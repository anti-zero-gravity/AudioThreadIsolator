[English](README.md) | [日本語](README.ja.md)

---

# Audio Thread Isolator (ATI)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/antizerogravity)

**Audio Thread Isolator (ATI)** は、Windows 上で動作するオーディオ再生アプリ、動画プレーヤー、ブラウザ、DAW（音楽制作ソフト）、ゲームなどの音声出力スレッドを自動走査・特定し、ユーザーによって指定された専用のCPUコアへ分離する軽量常駐ユーティリティです。

開発企図は、描画・デコード・通信などの煩雑で重い通常スレッドを、オーディオ専用コア以外へ退避させることで、オーディオスレッドへの競合・干渉を遠ざけた環境で得られる特有の音質を楽しむ為のアプリです。
MSI Affinity V9による、デバイスのAffinityMaskと優先度設定、MMCSSレジストリによる優先度設定と協調する環境を準備することで、オーディオスレッドへ割り込み・割り込まれを低減し、ブラウジング・ゲーム・その他のあらゆるシーンでPC音楽再生の音質向上を図る為のユーティリティーです。高度なAffinityMask・優先度変更機能も備えています。

---

## 主な特徴

- **低リソース常駐**
- **再生スレッドの自動判別と動的優先度表示**:
  - オーディオスレッドを特定して設定対象として記憶します。特定時には動的優先度を取得して表示します。
- **複数コアアフィニティ対応**:
  - 特定されたスレッドは複数コアに割り当て可能。
- **立体優先度ピッカーによる直感的な優先度変更**
- **最前面表示 (Always on Top) & ウィンドウリサイズ**
- **直感的な GUI とタスクトレイ常駐**:
  - 実行中のプロセス一覧からチェックボックスで対象アプリを追加。
  - ウィンドウ右上の「×」ボタンまたは最小化でシステムトレイ（通知領域）に収納。
  - Windows 起動時の自動スタートアップ登録に対応。
- **管理者権限モードで再起動にすればリアルタイム優先度設定やaudiodg.exeの管理が可能に**:
  - タスクトレイアイコンの右クリックメニュー「`Restart as Administrator`」からワンクリックで管理者権限へ昇格再起動。
  - OS 特権を自動有効化し、通常起動では制限される Windows カーネルのリアルタイム優先度クラス（Base Priority 16〜31）が設定可能。
  - **起動モードによる設定範囲の違い**:
    - PPL（Protected Process Light）で保護された`audiodg.exe`プロセスの優先度設定やコア隔離が可能になります。
    - Unlisted マスクを適用できるプロセス数が増えます。
- **監視対象外（Unlisted）プロセスの自動アフィニティ退避と個別除外**:
  - Global Settings の排他チェックボックス（`Default Audio Core` 除外 / `Excluded` マスク適用）を ON にすると、ATI 未登録の全プロセスへ `SetProcessAffinityMask` を一括適用し、以降に新規起動するプロセスも `INI×10` 周期で自動適用されます。累計の適用数は左下に`● <累計数> Unmonitored Processes Masked`と表示されるようになります。
  - `[Add]`（`Select Process`）画面の先頭 `!` カラムでチェックしたプロセスは、未登録全プロセス対象から除外できます。
  - 未登録の全プロセス監視を停止すると`Unmask All`が緑に点灯し、押せば全プロセスのマスクを一括解除できます。

---

## 動作の仕組み

ATI は登録されたプロセス内からオーディオ再生スレッドを識別・追従するため、以下の判定機能を用いています。

### 1. スレッド名のキーワード照合
- Windows 10 以降の OS 標準機能（`GetThreadDescription`）を用いて、プロセス内の全スレッドの名称を取得します。
- 以下の既知キーワードおよびパターンと照合します：
  - **メディアプレーヤー**: `ao/*`（例: `ao/wasapi`, `ao/pipewire`）。
  - **Chromium 系ブラウザ**: `wasapi_render_thread`, `AudioOutputDevice`, `AudioThread`, `CrAudio`。
  - **一般オーディオキーワード**: `audio`, `playback`, `wasapi`, `asio`。


### 2. モジュール照合（開始アドレス・スタック参照先）
- スレッド名が設定されていないアプリ（DAW、波形編集ソフト、ASIO 対応アプリなど）の場合、各スレッドの生成元（開始アドレス: `Win32StartAddress`）およびスタック参照先を照合します。
- スレッドがオーディオドライバや関連モジュールから起動されているかを判定します：
  - **ASIO ドライバ DLL**: Windows レジストリ（`HKLM\SOFTWARE\ASIO` 等）からシステムにインストールされている実 ASIO ドライバ DLL を自動取得して照合します。
  - **Windows オーディオエンジン関連 DLL**: `audioses.dll`, `dmusic.dll`, `avrt.dll` 等と照合します。

### 3. CPU サイクル差分サンプリング
- スレッド名等で即座に特定できない場合、一定ポーリング間隔でスレッドの CPU サイクル消費量（`QueryThreadCycleTime`）を計測し、稼働中のオーディオスレッドを判定します。

### 4. アプリケーション種別ごとの専用制御
- **Windows オーディオエンジン (`audiodg.exe`)**:
  - 内部スレッドが短時間で頻繁に入れ替わるため、プロセス全体（PID）にオーディオ専用コアマスク・プロセス優先度クラス・HighQoS を適用し、配下の全スレッド（全 TID）へ指定優先度を適用します（※ `LOCAL SERVICE` 権限で動作するため管理者権限起動が必要）。
- **Chromium 系ブラウザ**:
  - 内部の Audio Service プロセスを特定し、音声送出スレッドを追従します。
- **Voicemeeter**:
  - ネットワーク送受信スレッドとメインオーディオエンジンスレッドの 2 行へ自動展開して個別に制御します。
- **Roon**:
  - PCM 送出スレッドを特定して単一スロットで制御します。

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
- **Unlisted プロセスへのマスク自動適用（`Apply` 排他チェック / `Unmask All`）**:
  - Global Settings 右側の `Apply` チェックボックス（上段: Default Audio Core 除外、下段: Excluded マスク適用）にチェックを入れると、監視リスト外の全プロセスへ強制アフィニティマスクを適用し、新プロセス起動時も自動追従します。
  - 監視リストには入れないがマスク対象からも外したいプロセスがある場合は、`[Add]`（`Select Process`）の先頭にある赤い **`!` カラム**にチェックを入れて `[Done]` を押すことで個別に除外できます。
  - 詳細な UI・設定仕様については [機能仕様書 (docs/spec_ja.md)](docs/spec_ja.md) を参照してください。

### 3. スレッド優先度と音質の設計思想
- **低優先度駆動（Idle）の優位性**:
  - 「優先度 1 が最もよい音質が得られる」という設定方針に則れば、スレッド処理が後回しにされた結果のスタッターや音切れが予想されます。クォンタム設定は ["Short / Variable / 1:1"](https://hackmd.io/@PsD5syoNTWq95HbQ6lFbtA/rkssS5LdGl#2-3-%E3%82%AF%E3%82%A9%E3%83%B3%E3%82%BF%E3%83%A0%E9%95%B7%E3%83%86%E3%83%BC%E3%83%96%E3%83%AB%E3%81%A8%E7%94%A8%E9%80%94%E5%88%A5%E3%83%81%E3%83%A5%E3%83%BC%E3%83%8B%E3%83%B3%E3%82%B0%E8%A8%AD%E5%AE%9A%E6%97%A9%E8%A6%8B%E8%A1%A8) を強く推奨します。
- **幅広いニーズへの対応**:
  - ATI は「音切れしない範囲で最も低い優先度が音質に優れる」という思想をベースに設計されていますが、プロセス優先度をアイドルに押さえて、オーディオスレッドのみを優先度31にしたり、その逆、プロセスをリアルタイムにしてオーディオスレッド優先度を１にするような極端な設定も可能です。
  - オーディオ用でも、その他全てのコアでもない、マスクのコアナンバーにチェックを入れないコアを作れます。この空きコアは複数のプロセスを渡るシグナルパスを想定していて、例えば#4 再生アプリ→#5 Voicemeeter→#6 audiodg→#7 LANデバイス→他PC　というような1プロセス1コア占有を可能にしています。

### 4. Tips（特定アプリケーションの設定例）
- **Roon**:
  - Roon は複数のプロセス（`Roon.exe`, `RoonAppliance.exe`, `RoonServer.exe`, `RAATServer.exe`）で構成されています。
  - **サーバー PC（Roon Server / Core 側）**: 常駐させた ATI に `RoonAppliance.exe` を登録します。
  - **レンダラー PC（Endpoint / Roon Bridge 側）**: 常駐させた ATI に `RAATServer.exe` を登録します（`RoonBridge.exe` を登録した場合は自動で `RAATServer.exe` へフォールバックします）。
  - （※`Roon.exe` は GUI、`RoonServer.exe` は管理サービスのため、音声処理スレッドは保持していません）
- **Voicemeeter + VBAN Receptor（ネットワーク再生構成）**:
  - サーバー PC に Voicemeeter、レンダラー PC に VBAN Receptor を登録して両スレッドを低優先度で動作させる場合の推奨環境:
    - **ネットワーク接続**: サーバー PC とレンダラー PC 間を Ethernet LAN ケーブル直結
    - **NIC 詳細設定**: 各種オフロードは「有効」、フロー制御は「無効」、デュプレックス（速度と二重方式）は「10M FULL」
    - **ATI コア割り当て**: Voicemeeter の `VAIO/AUX & DSP` 項目に対する Audio Core 割り当て CPU の推奨数は「4個以上」（例: 16進マスクによる複数コア割り当て）

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

ビルドが完了すると、ルートディレクトリにスタンドアロン実行バイナリ `ATI.exe`（約 4.1MB）が生成されます。

---

## ライセンス
MIT License.
