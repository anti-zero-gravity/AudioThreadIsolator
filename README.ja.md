[English](README.md) | [日本語](README.ja.md)

---

# Audio Thread Isolator (ATI)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/antizerogravity)

**Audio Thread Isolator (ATI)** は、Windows 上で動作するオーディオ再生アプリ、動画プレーヤー、ブラウザ、DAW（音楽制作ソフト）などのリアルタイム音声出力スレッド（WASAPI / ASIO 等）を自動走査・特定し、指定された専用の CPU コアへ固定（Isolate）する軽量常駐ユーティリティです。

再生スレッドを単独または専用の複数コアへ隔離し、同時に描画・デコード・通信などの重い通常スレッド群を別コア群へ退避させることで、CPU コアの競合によるバッファアンダーラン（音途切れ）とマイクロジッターを極小化します。

---

## 主な特徴

- **低リソース常駐**:
  - C++17（Pure Win32 API / Common Controls）ネイティブ実装。
  - .NET ランタイムや外部 DLL 不要の単一バイナリ（常駐メモリ約 1〜2MB、CPU 負荷 0%）。
- **3段階の再生スレッド自動判定エンジン**:
  - メディアプレーヤー（mpv 等）、Chromium 系ブラウザ（Vivaldi / Chrome 等）、DAW・オーディオエディタ（iZotope RX 等）の再生スレッドを自動判別。
- **複数コアアフィニティ対応**:
  - 負荷の高いオーディオ処理や音切れ対策として、再生スレッドを 1 コアだけでなく 2 コア以上の複数コア（例: `#1` や `#1, #2`）へ柔軟に割り当て可能。
- **最前面表示 (Always on Top) & ウィンドウリサイズ対応**:
  - メイン画面右上に常設されたチェックボックスにより、即座に最前面表示をオン／オフ切り替え可能（設定は自動保存）。
  - ウィンドウ枠を掴んで自由に縦横伸縮可能（最小サイズ制限によりレイアウト破綻を防止）。
- **直感的な GUI とタスクトレイ常駐**:
  - 実行中プロセス一覧からチェックボックスで簡単に対象アプリを追加。
  - ウィンドウ右上の「×」ボタンまたは最小化でシステムトレイ（通知領域）に収納。
  - Windows 起動時の自動スタートアップ登録に対応。
- **管理者権限起動でリアルタイム優先度（Base 16〜31）が指定可能**:
  - タスクトレイアイコンの右クリックメニュー「`Restart as Administrator`」からワンクリックで管理者権限へ昇格再起動。
  - OS 特権（`SeIncreaseBasePriorityPrivilege`）を自動有効化し、通常起動では制限される Windows カーネルのリアルタイム優先度クラス（Base Priority 16〜31）が 2D Priority Matrix Picker から選択・即時適用可能（※管理者権限でない場合は High priority にサイレントでフォールバック）。

---

## 動作の仕組み

ATI は未知のアプリケーションであっても確実に再生スレッドを特定・追従できるよう、以下の 3段階の判定エンジンを搭載しています。

### 1. 第 1 判定: スレッド名のキーワード走査
- Windows 10 以降の OS 標準機能（`GetThreadDescription`）を用いて、プロセス内の全スレッドの名称を走査します。
- 以下の既知キーワードおよびパターンと照合します：
  - **メディアプレーヤー**: `ao/*`（例: `ao/wasapi`, `ao/pipewire`）。
  - **Chromium 系ブラウザ**: `wasapi_render_thread`, `AudioOutputDevice`, `AudioThread`, `CrAudio`。
  - **一般オーディオキーワード**: `audio`, `playback`, `wasapi`, `asio`。
- ※ `audio.CrUtilityMain` や `watchdog` などの制御・監視用メインスレッドは誤検出防止フィルタにより明示的に除外されます。

### 2. 第 2 判定: モジュール開始アドレスの照合
- スレッド名が設定されていない専門ソフト（DAW、波形編集ソフト、ASIO 対応アプリなど）の場合、各スレッドの生成元（開始アドレス: `Win32StartAddress`）を照合します。
- スレッドがオーディオドライバや関連 DLL から起動されているかを判定します：
  - ASIO ドライバ DLL（例: `*asiodriver*.dll`, `vbvmaux_asiodriver64.dll` 等）。
  - Windows オーディオエンジン関連 DLL（`audioses.dll`, `dmusic.dll` 等）。

### 3. 第 3 判定: コールスタック逆アセンブル走査 & 強化型ヒューリスティックサンプリング
スレッド名が設定されていないゲームエンジン（Godot, Unity）やレトロ・インディーズゲーム（DirectSound / WinMM 採用作品等）において、最新の静的・動的解析技術を組み合わせて真のオーディオスレッドを確実に特定します：

- **スタック底内部構造体 & コールスタック逆アセンブル走査**:
  - **FMOD / Unity**: スレッド TEB の `StackBase` 手前メモリを走査し、内部構造体に平文で埋め込まれたスレッド名（`FMOD (WASAPI) feeder thread` 等）を直接抽出。
  - **Godot Engine 等 (WASAPI)**: スレッドのスタックに積まれたリターンアドレスから関数コード近傍（`[rip + disp32]` 相対参照等）を走査し、平文を持たない無名スレッドから `"audio_driver_wasapi"` / `"AudioDriverWASAPI"` などの決定打シンボルを直接検出（初回 1 ターン目で Rank 95 として即時特定・隔離）。
- **強化型 10秒ヒューリスティックサンプリング (モジュール無制限 / 最上 delta 除外)**:
  - 従来の狭い DLL ホワイトリストを全廃し、外部サウンドモジュール（`DSOUND.dll`, `WINMM.dll`, `pxtoneWin32.dll` 等）を含む全スレッドを探索対象化。
  - プロセス内の全スレッドを 10 秒間（20 ターン）サンプリング監視し、累積 delta を集計。
  - **負荷第 1 位（最上 delta スレッド: メインループや描画スレッド）を構造的に除外**。
  - **第 2 位 〜 第 10 位の活動スレッド群を候補**として、オーディオモジュールシグネチャや定常周期（10ms〜15ms オーダー）を検査し、本命オーディオスレッドを自動特定・隔離します。
- **グラフィックドライバ系などでCPUコアが固定されて隔離できない同居スレッドの優先度抑制と保護**:
  - 通常スレッドを通常コア群へ退避させた後、実際にアフィニティ（コア割り当て）を物理確認し、グラフィックドライバ（NVIDIA 等）などの仕様でCPUコアが固定されオーディオ専用コアに残ってしまうスレッドを検出。
  - 正常退避された通常スレッド（メイン、描画、入力等）の優先度には一切触れず、ゲームやOS本来の優先度をそのまま維持（意図しない優先度低下を防止）。
  - このような隔離できないスレッドがオーディオ専用コアに同居している場合（Half-Isolated）、オーディオスレッドを必要に応じて自動昇格（`-15` ➔ `-2` Lowest）させ、同居スレッド側をその直下階層（`-15` 等）へ自動抑制することで、「オーディオスレッド ＞ 同居スレッド」の優先関係を維持し音切れを防止します。

### 前回起動時に検出済みのPID/TIDトラッキング機構
- 一度オーディオスレッドとして特定・隔離されたスレッド（TID）やプロセス（PID）は、ATI のトラッキングテーブルおよび INI にキャッシュ保持されます。
- 次回起動時の高速復元に活用されるほか、ATI が当該スレッドの優先度を変更（例: `Idle (-15)` へ設定）した後も同一プロセス内で追尾を継続し、判定の反転（ピンポン現象）を防ぎます。
---

## 使用方法と推奨環境の構築

### 1. 動作の前提（オーディオ専用 CPU# の準備）
ATI のデフォルト設定では、登録されたアプリの起動を検知すると、プロセス内のオーディオスレッドのみ、デフォルト設定値である CPU#1 へ保護され、プロセス内の（GUI 描画スレッドなど）その他のすべてのスレッドは #1 以外の CPU# へ OS 判断で分散退避されます。

ここで前提になるのが、マルチコア・マルチスレッド CPU において**オーディオ以外に使用されない CPU#（ナンバー）**を準備することです。

1. **デバイス割り込み（MSI）の退避**:
   - [MsiAffinityUtility-v9](https://github.com/anti-zero-gravity/MsiAffinityUtility-v9) を使用し、グラフィックボードを含むすべての MSI 対応デバイス（MSI非対応デバイスについては未検証）の割り込み先を、自分でオーディオ専用と決めた任意の CPU# から退避させ、直後に OS を再起動する下準備が必要です。
2. **CPU# 選定の目安**:
   - オーディオスレッド用 CPU# には、#0 以外（ハイパースレッディング有効時は #0 と #1 以外）を選んでください。
3. **オンボードおよびネットワークオーディオ構成時の配慮**:
   - **オンボード HD-Audio**: オーディオスレッド用 CPU# を指定、または HD-Audio 専用 CPU# を用意することをおすすめします。
   - **LAN 経由送信 (NAA 等)**: LAN 経由でオーディオデータを NAA（Network Audio Adapter）などに送信するオーディオ構成の場合は、LAN 専用の CPU# を選定しておくことをおすすめします。

### 2. ATI の初期設定
- **DefaultAudioCore**:
  - 事前に任意設定したオーディオ専用 CPU#（複数指定可能）を設定します。
- **ExcludedCores**:
  - （LAN用含む）オーディオスレッド用 CPU# 以外のすべてのプロセスとスレッドに許可する CPU# を設定します。

### 3. スレッド優先度と音質の設計思想
- **低優先度駆動（Idle）の優位性**:
  - 「優先度 1 が最もよい音質が得られるのではないか」という設計思想のため、デフォルト設定のオーディオスレッド優先度には `Idle (-15)` が設定されています。クォンタム設定は ["Short / Variable / 1:1"](https://hackmd.io/@PsD5syoNTWq95HbQ6lFbtA/rkssS5LdGl#2-3-%E3%82%AF%E3%82%A9%E3%83%B3%E3%82%BF%E3%83%A0%E9%95%B7%E3%83%86%E3%83%BC%E3%83%96%E3%83%AB%E3%81%A8%E7%94%A8%E9%80%94%E5%88%A5%E3%83%81%E3%83%A5%E3%83%BC%E3%83%8B%E3%83%B3%E3%82%B0%E8%A8%AD%E5%AE%9A%E6%97%A9%E8%A6%8B%E8%A1%A8) を強く推奨します。
- **環境に応じた柔軟な再設定**:
  - お使いの Windows の状況により音切れが頻発することが考えられるため、default setting だけでなくプロセス個別にプロセス／オーディオスレッド優先度を独立して再設定できます。
- **幅広い音質比較ニーズへの対応**:
  - ATI は「音切れしない範囲で最も低い優先度が音質に優れる」という思想をベースに設計されていますが、最高優先度である Realtime（Base 31）まで即座に切り替えられる柔軟性も備えています。優先度の違いによる音質特性の変化をリアルタイムに聴き比べるなど、幅広い試聴ニーズや好みに応じた運用が可能です。

---

## ダウンロード

コンパイル済みのスタンドアロン実行バイナリ（`ATI.exe`）は、[Releases](https://github.com/anti-zero-gravity/AudioThreadIsolator/releases) ページから直接ダウンロードできます。

---

## 開発環境・ビルド

### 動作要件
- **OS**: Windows 10 / Windows 11 (64-bit)
- **コンパイラ**: MinGW-w64 `g++` (C++17 / UCRT 対応) および `windres`, `strip`

### 開発環境未導入の一般環境からのセットアップ手順
開発環境がインストールされていない初期状態の Windows や、AI エージェントにビルドを依頼する場合は、Windows 標準パッケージマネージャー（`winget`）を用いて以下の手順でコンパイラを導入します。

1. **MinGW-w64 (WinLibs UCRT) の自動インストール**:
   PowerShell から以下を実行します：
   ```powershell
   winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e --accept-source-agreements --accept-package-agreements
   ```
2. **環境変数の反映**:
   インストール完了後、PowerShell を再起動するか、現在のセッションの PATH を更新します：
   ```powershell
   $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
   ```
3. **正常導入の確認**:
   `g++ --version` を実行し、バージョンが表示されれば準備完了です。

### ビルド実行手順
リポジトリ直下の自動ビルドスクリプトを実行します（リソース結合、静的リンク、バイナリ軽量化を一括実行します）：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

※ スクリプトを用いずに直接コマンドを実行する場合は以下の通りです：
```powershell
mkdir build -ErrorAction SilentlyContinue
windres src/resource.rc -O coff -o build/resource.res
g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ src/main.cpp src/process_picker.cpp src/isolator.cpp src/priority_matrix_picker.cpp build/resource.res -lcomctl32 -lshlwapi -ldwmapi -lpsapi -lgdiplus -o ATI.exe
strip ATI.exe
```

ビルドが完了すると、ルートディレクトリに完全単一のスタンドアロン実行バイナリ `ATI.exe`（約 4.1MB）が生成されます。

---

## ライセンス
MIT License.
