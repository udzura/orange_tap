# Ruby メソッドトレーシング Gem "RedFaucet" 設計概要 & AI生成プロンプト

## 1. 目的

Rubyプロセス内のメソッド呼び出し（call/return）を低オーバーヘッドでフックし、
中央デーモンプロセスへ非同期に送信、最終的にOpenTelemetry(OTel)形式のSpanとして
構築するためのRuby gem（ネイティブ拡張）を作成する。

対象OS: macOS
対象Ruby: MRI (CRuby)

---

## 2. アーキテクチャ全体像

```
┌─────────────────────────────┐
│  監視対象 Rubyプロセス          │
│                              │
│  rb_add_event_hook           │  ← VMのCALL/RETURNイベントを直接フック
│       ↓                      │     (Rubyオブジェクト生成なし、数値のみ扱う)
│  ring_buffer_t (共有メモリ)    │  ← ロックフリーな固定長リングバッファに書き込み
│       ↑                      │
│  XPCクライアント                │  ← 起動時に一度だけ、共有メモリ参照(Mach send right)を
└──────────┬───────────────────┘     xpc_shmem_create経由でデーモンに渡す
           │ XPC (制御プレーン: 接続確立・シンボルテーブル送付・終了通知)
           ↓
┌─────────────────────────────┐
│  中央デーモンプロセス            │
│                              │
│  XPCサーバ                    │  ← 複数Rubyプロセスからの接続を受け付け
│       ↓                      │
│  ring_buffer_t リーダー         │  ← プロセスごとのリングバッファをポーリング
│       ↓                      │
│  CALL/RETURN 対応付け           │  ← スレッドID単位でスタック管理
│       ↓                      │
│  payload → OTel Span 変換      │  ← 既存実装を流用（差し替え可能なインターフェースとして呼ぶ）
│       ↓                      │
│  OTLP Export / 保存            │
└─────────────────────────────┘
```

### レイヤーごとの役割分担

| レイヤー | 技術 | 役割 | 頻度 |
|---|---|---|---|
| フック取得 | `rb_add_event_hook` | VM内call/returnイベントの捕捉。Rubyブロック呼び出しを経由しないため最小オーバーヘッド | 高頻度（ホットパス） |
| データプレーン | `ring_buffer_t`（共有メモリ, `mmap`+`MAP_SHARED`） | フックで取得したイベントをロックフリーに書き込み | 高頻度（ホットパス） |
| 制御プレーン | XPC（方式B: `xpc_shmem_create`/`xpc_shmem_map`） | 接続確立、共有メモリのMach send right受け渡し、シンボルテーブル送付、終了通知 | 低頻度 |
| 変換 | 既存実装（流用） | payload → OTel Span 変換 | デーモン側、頻度はイベント数に比例するが本設計のスコープ外 |

---

## 3. コンポーネント詳細

### 3.1 イベント構造体（固定長・POD）

```c
typedef enum {
    TRACE_EVENT_CALL   = 0,
    TRACE_EVENT_RETURN = 1,
} trace_event_type_t;

typedef struct {
    uint64_t timestamp_ns;   // mach_continuous_time() ベース（Cレベルで直接取得）
    uint32_t thread_id;      // pthread_self() 由来の数値化ID
    uint32_t event_type;     // trace_event_type_t
    uint64_t probe_id;       // メソッドを一意に識別する数値ID（Symbol/method_id をCレベルの
                              // ハッシュテーブルで事前に採番したもの。文字列は載せない）
    uint64_t call_id;        // CALLとRETURNを対応付けるための一意な値
                              // （スレッドローカルなカウンタから生成）
} trace_event_t; // 32 bytes固定、パディングに注意してalignasを検討
```

### 3.2 共有メモリ・リングバッファ

```c
typedef struct {
    _Atomic uint64_t write_index;   // probe(Ruby)側がインクリメント
    _Atomic uint64_t read_index;    // デーモン側がインクリメント
    uint64_t capacity;              // スロット数（2の冪を推奨、マスク演算のため）
    _Atomic uint64_t dropped_count; // バッファあふれで上書きされたイベント数（監視用）
    trace_event_t events[];         // フレキシブル配列メンバ
} ring_buffer_t;
```

- SPSC（単一Writer=フック、単一Reader=デーモン）を前提としたロックフリー設計。
- あふれた場合は古いイベントを上書き。`dropped_count`で検知可能にする。
- `capacity`・確保サイズは初期化時（gemロード時）に確定し、可変長拡張は行わない。
- Writer側（Rubyプロセス内のフック）は **一切シスコールを発生させない**（アトミックなインデックス更新のみ）。
- Reader側（デーモン）はポーリング方式（数ms間隔）を基本とする。低頻度イベント即時性が必要な場合のみ、
  `dispatch_source`等による軽量通知を検討（本設計では必須要件としない）。

### 3.3 共有メモリの確保と受け渡し（方式B: Mach send right経由）

```
[Rubyプロセス側 Init_xxx()]
  1. mach_vm_allocate でメモリ領域を確保
  2. xpc_shmem_create(addr, size) でXPCオブジェクト化
  3. XPC接続確立 → xpc_dictionary に shared_memory オブジェクトを積んで送信
  4. 以降、その仮想アドレスに ring_buffer_t* をキャストして直接読み書き

[デーモン側 XPCハンドラ]
  1. 受信メッセージから shared_memory オブジェクトを取り出す
  2. xpc_shmem_map(shm_obj, &addr, &size) でマップ
  3. ring_buffer_t* にキャストしてポーリング対象に登録
```

- 方式A（`shm_open`で名前を払い出し、名前文字列だけをXPCで送る）はより実装が単純だが、
  同一ユーザーの他プロセスからも名前を知っていれば`shm_open`でアクセスできてしまう点に留意。
  → 今回は方式B（Mach send right受け渡し）を採用するため、この懸念は該当しない。
- 方式Bはデーモン再起動時に既存プロセスと自動再接続できない制約がある。
  再接続が必要な場合、Rubyプロセス側でXPC接続断を検知し再送する仕組みを検討する
  （本gemの初期スコープでは「再接続ロジックの有無」を設定可能にする程度に留めてよい）。

### 3.4 制御プレーンで送るその他の情報

- **シンボルテーブル**: `probe_id → "ClassName#method_name"` のマッピング。
  トレース対象登録時（gem初期化時）に一度だけXPCメッセージ（`xpc_dictionary`）で送付。
- **プロセスメタデータ**: `pid`, プロセス起動時刻, Rubyバージョン等。
- **終了通知**: プロセス終了時（`at_exit`フック等）にXPCで明示的に通知し、
  デーモン側でのリングバッファ登録解除・リソース解放を行う。

### 3.5 フック実装（`rb_add_event_hook`）

```c
static void
event_hook(VALUE tpval, void *data)
{
    rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval);
    rb_event_flag_t event = rb_tracearg_event_flag(trace_arg);
    ID mid = rb_tracearg_method_id(trace_arg);

    // 対象外メソッドの即時フィルタ（Cレベルのハッシュテーブル参照のみ、Rubyオブジェクト生成なし)
    uint64_t probe_id;
    if (!lookup_probe_id(mid, &probe_id)) return;

    uint64_t idx = atomic_fetch_add(&g_ring->write_index, 1) % g_ring->capacity;
    trace_event_t *ev = &g_ring->events[idx];
    ev->timestamp_ns = mach_continuous_time_ns();
    ev->thread_id     = current_thread_numeric_id();
    ev->event_type    = (event & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL)) ? TRACE_EVENT_CALL : TRACE_EVENT_RETURN;
    ev->probe_id      = probe_id;
    ev->call_id       = next_call_id(ev->thread_id, ev->event_type);
}

void
Init_red_faucet(void)
{
    // 初期化: 共有メモリ確保、XPC接続確立、シンボルテーブル構築
    setup_shared_memory();
    setup_xpc_connection();
    rb_add_event_hook(event_hook, RUBY_EVENT_CALL | RUBY_EVENT_RETURN | RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN, Qnil);
}
```

**厳守事項:**
- フック内でRubyオブジェクトを新規生成しない（`tp.binding`, `tp.parameters`, 文字列生成, Hash/Array生成は禁止）。
- フック内で例外送出・ブロッキングI/O・ロック取得を行わない。
- タイムスタンプ取得もCレベル（`mach_continuous_time`）で完結させ、Rubyの`Process.clock_gettime`は使わない。

### 3.6 対象メソッドの絞り込み（What: どのメソッドを追跡するか）

- Cレベルで`lookup_probe_id`によるフィルタを行う（3.5参照）ため、`TracePoint#enable(target:)`は使用しない
  （`rb_add_event_hook`にはtarget絞り込みAPIが無いため）。
- 対象メソッドの登録は、gemのRuby側APIから行う想定：

```ruby
RedFaucet.trace_method(MyClass, :my_method)
RedFaucet.trace_all_instance_methods(MyOtherClass)
```

- 登録内容はCエクステインション内のハッシュテーブルに反映し、フックの判定に使う。
- これは「常時どのメソッドを監視対象にするか」という**静的な設定**であり、
  次項3.8の「いつ実際に記録するか（セッション制御）」とは独立した軸である。

### 3.7 記録セッションの制御（When: いつ記録するか）— 公開API

要求仕様に基づき、Rubyレベルの公開APIは「記録区間（セッション）」をブロック形式・
インスタンス形式の2通りで制御できるようにする。

```ruby
# ブロック形式
RedFaucet.throw do
  # この区間で発生したCALL/RETURNイベントが1つのセッションとして記録される
end
# => 生成されたSpan群のJSONファイルパス（String）が返却値になる

# インスタンス形式
tape = RedFaucet.new
tape.throw
# この区間はSpanが作られる
tape.stop
# => 生成されたSpan群のJSONファイルパス（String）が返却値になる
```

**設計上のポイント:**

- `throw`/`stop`は「どのメソッドを対象にするか(3.6)」ではなく、
  「今からイベントを記録し、セッション終了時にSpanへ変換して出力する」という
  **記録区間のライフサイクル制御**を担う。3.6の`trace_method`等での登録は
  事前に（あるいはセッション開始前に）済ませておく前提。
- `throw`呼び出し時にセッションID（`session_id`, 例: UUIDまたはモノトニックなカウンタ）を
  発行し、以降のイベント書き込み（`trace_event_t`）に`session_id`を含める
  （3.1のイベント構造体に`session_id`フィールドを追加する必要がある。下記3.7.1参照）。
- `stop`（またはブロック終了時）に、XPC経由で**同期的に**デーモンへ
  「このsession_idのセッションを終了し、Span JSONを生成してほしい」という
  リクエストを送信し、**デーモン側の処理完了を待って応答（ファイルパス）を受け取る**。
  つまりこの呼び出しはXPCの非同期メッセージではなく、応答を待つ同期的なリクエスト
  （`xpc_connection_send_message_with_reply`等）を用いる。
- ブロック形式(`RedFaucet.throw do ... end`)は内部的に「セッション開始 → `yield` →
  （例外発生時も含め`ensure`で）セッション終了・ファイルパス取得 → 返却」を行う
  シンタックスシュガーであり、インスタンス形式(`throw`/`stop`)と同じ内部実装を共有する。
- ネストしたセッション（`throw`のブロック中にさらに`throw`する等）を許容するかは
  初期スコープでは**非対応**とし、二重呼び出し時は例外を送出する方針を基本とする
  （将来的に子セッション＝子Traceとして扱う拡張は可能）。

#### 3.7.1 イベント構造体・共有メモリ設計への影響

3.1の`trace_event_t`に`session_id`を追加する（あるいは`call_id`の上位ビットに埋め込む等、
32 bytes固定を維持したい場合は工夫が必要）。セッションに属さないイベント
（`throw`前や`stop`後に発生したイベント）は`session_id == 0`等の番兵値として扱い、
デーモン側で無視・破棄してよい。

```c
typedef struct {
    uint64_t timestamp_ns;
    uint32_t thread_id;
    uint32_t event_type;
    uint64_t probe_id;
    uint64_t call_id;
    uint64_t session_id;   // 追加: 0 = セッション外（記録対象外）
} trace_event_t; // 40 bytes固定
```

- 記録対象外(`session_id == 0`)のイベントをリングバッファに書き込むかどうかは
  性能とのトレードオフ。**推奨は「フック内でsession_idが0ならバッファ書き込み自体を
  スキップする」**（3.5のフィルタ処理に、対象メソッド判定と並んで
  「現在アクティブなセッションが存在するか」の判定を追加する）。
  これにより`throw`区間外のオーバーヘッドを実質ゼロに近づけられる。
- 現在アクティブな`session_id`は、フック内から高速に参照できる場所
  （プロセス内グローバル変数、Cレベルの`_Atomic uint64_t`）に保持する。
  スレッドをまたいだ記録（別スレッドで発生したCALL/RETURNも同一セッションに含めたい場合）を
  サポートするなら、この変数はプロセス全体で共有し、`throw`時にセットする設計でよい
  （インスタンス単位で並行して複数セッションを走らせる要件が出た場合は、
  スレッドローカルまたはインスタンスごとの管理へ拡張する）。

### 3.8 デーモン側: CALL/RETURN対応付け → OTel変換 → ファイル出力

```
スレッドID単位・session_id単位でコールスタック(配列)を保持
CALL受信  → PendingSpan生成、スタックにpush（parent_span_idは直前のスタックトップ）
RETURN受信 → スタックからpop、end_timeを確定
           → 既存の payload -> OTel 変換関数を呼び出す（差し替え可能なインターフェースとして実装）

「セッション終了」リクエスト受信(session_id指定) →
    そのsession_idに属するSpan群を確定・シリアライズ(JSON) →
    ファイルに書き出し →
    ファイルパスをXPCの同期応答として返す
```

- 既存の`payload → OTel`変換実装をそのまま呼べるよう、デーモン側では
  「CALL/RETURNペアを解決した時点のデータ構造」を**既存実装が期待する入力フォーマットに合わせて**
  組み立てるアダプタ層を用意する（既存実装の入力仕様に応じて調整）。
- 異常系（RETURN未着、再帰、例外による巻き戻し）はスタックの整合性チェックで検知し、
  不整合時はエラーカウンタに記録して該当Spanをdropする方針とする。
- セッション終了時点で対応するRETURNが来ていないCALL（`stop`時点で呼び出しが完了していない
  非同期処理など）をどう扱うか（強制クローズしてend_timeを`stop`時刻にする／該当Spanを
  不完全としてマークする等）は実装方針として明記しておく。
- 出力ファイルのパス・命名規則（例: `<tmpdir>/red_faucet/<session_id>.json`）は
  設定可能にしておくと運用しやすい。

---

## 4. Gemの構成（想定ディレクトリ構造）

```
red_faucet/
├── red_faucet.gemspec
├── lib/
│   ├── red_faucet.rb              # Rubyレベルの公開API (throw/stop, trace_method等)
│   └── red_faucet/
│       └── version.rb
├── ext/
│   └── red_faucet/
│       ├── extconf.rb
│       ├── red_faucet.c           # Init_red_faucet, rb_add_event_hook 登録
│       ├── ring_buffer.c / .h      # 共有メモリ・リングバッファ実装
│       ├── xpc_client.c / .h       # XPC接続・shmem受け渡し・シンボルテーブル送付
│       └── probe_table.c / .h      # probe_id採番・ハッシュテーブル管理
├── spec/                           # RSpec等によるテスト
└── README.md                       # 方式A/Bの選定理由、macOS前提、既知の制約を明記
```

---

## 5. README に明記すべき留意事項

- 本gemはmacOS専用（XPC / Mach VM APIに依存）である旨。
- 共有メモリの受け渡しは方式B（`xpc_shmem_create`/`xpc_shmem_map`）を採用しており、
  XPC接続を持つプロセスにのみメモリ実体が渡る。名前ベース（`shm_open`名前渡し）方式は
  採用していないため、同一ユーザーの他プロセスから共有メモリ名を推測してアクセスされる懸念はない。
- デーモン再起動時、既存のRubyプロセスとの再接続は自動では行われない制約がある
  （必要な場合は再接続ロジックの実装状況を明記）。
- フック内部でRubyオブジェクトを生成しない設計のため、`probe_id`とメソッド名の対応は
  起動時に送付したシンボルテーブルをデーモン側で保持する前提になっている。
- リングバッファがあふれた場合は古いイベントから上書きされ、`dropped_count`で検知可能。

---

## X. 残課題（実装着手前に決めておくと良い事項）

- `probe_id`の採番方式（プロセスごとにローカルか、クラス名+メソッド名のハッシュ値で
  グローバルに決定的にするか）。後者なら複数プロセスのSpanをデーモン側で名寄せしやすい。
- マルチスレッドRubyアプリケーションでの`call_id`生成方式（スレッドローカルカウンタ vs
  グローバルアトミックカウンタ）。
- 既存の`payload → OTel`変換実装が期待する入力フォーマット（イベント単位かSpan単位か）。
  デーモン側アダプタ層の設計に直結するため、早めに確認しておくとスムーズです。
- `throw`/`stop`のXPC同期リクエストにタイムアウトを設けるか（デーモンが応答しない場合、
  Ruby側の`stop`呼び出しがハングし続けるリスクがあるため）。
- 複数スレッドが同時に`RedFaucet.new.throw`する場合の扱い（並行セッションを許容するか、
  プロセス全体で1セッションのみに制限するか）。許容する場合、3.7.1のアクティブ
  session_id管理をスレッドローカルもしくはインスタンス単位に拡張する必要がある。
- 生成されたJSONファイルの保存先・保持期間・クリーンアップ方針
  （呼び出し側で都度削除するのか、gem側で一定期間後に自動削除するか）。