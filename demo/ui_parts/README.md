# Priority Matrix 3D UI Assets & Hit-Test Map

Windows の Process Class × Thread Priority (7×7 = 49 マトリクス) を 3D 透視投影でビジュアル選択するための UI パーツ一式です。

## ディレクトリ構成
```
ui_parts/
├── base_layer.png          # 【原画】全柱 45% 輝度（背景透過） [58 KB]
├── glow_masks/             # 【差分マスク 49枚】選択柱＋同値柱のみ描画 [各約 8 KB]
│   ├── mask_rank_01.png
│   ├── ...
│   └── mask_rank_49.png
├── full_composites/        # 【完全合成版 49枚】1枚ロード用 [各約 25 KB]
│   ├── matrix_rank_01.png
│   └── ...
├── hit_map.png             # 【当たり判定PNG】ピクセル値 (0〜49) のインデックス画像 [5 KB]
├── hit_map.bin             # 【最速テーブル】282×340 の生バイト配列 [93 KB]
└── README.md
```

## 全画像の共通仕様
* **解像度**: **`282 × 340 px`**（原画・マスク・当たり判定マップすべて完全に同一サイズ）
* **アングル**: ズーム `0.56x` ｜ 手前 `+27.9°` ｜ 左奥 `+116.1°` ｜ 仰角 `47.0°`（3点透視投影）

---

## マウスホバー・クリック当たり判定の方式比較

| 方式 | ファイル容量 | CPU計算コスト | 実装の容易さ | 3D凹凸・隠蔽判定の精度 |
| :--- | :--- | :--- | :--- | :--- |
| **① カラーインデックス・マップ方式【採用】** | **5 KB (PNG) / 93 KB (bin)** | **O(1) (約 1 ナノ秒)** | **極めて簡単 (`arr[y, x]`)** | **100% 完璧 (ピクセル単位)** |
| ② 49角形ポリゴン判定方式 | 数十 KB (JSON) | O(N) (毎フレーム49回判定) | 複雑 (GraphicsPath等) | 重なりの内外判定が破綻しやすい |
| ③ 3D Raycasting 方式 | 数百 KB〜数 MB (3Dエンジン要) | 高負荷 (3D交差計算) | C#での3D実装が必要 | 良好だがコードが肥大化 |

### なぜ「カラーインデックス・マップ（方式①）」が圧倒的に最適なのか？
1. **超低容量**: 画像でわずか **5 KB**、生配列でも **93 KB**。
2. **超低コスト**: `MouseMove` イベント内で `byte rank = hitMap[y * 282 + x];` と配列を 1 回参照するだけ（所要時間 1 ナノ秒、CPU 負荷 0%）。
3. **完全な精度**: 3D GPU レンダラーのデプスバッファ（手前の柱が奥の柱を隠す境界線）をそのままピクセル値として焼き込んであるため、**1 ピクセルの狂いもなく完全に自然なホバー判定**が実現します。
   * ピクセル値 `0`: 背景（柱の外側）
   * ピクセル値 `1`〜`49`: その座標にある柱の **Rank 番号 (1〜49)**

---

## C# (WinForms) 完全実装サンプルコード

マウスホバー時にリアルタイムでグロー表示を切り替え、クリックで選択を確定する標準実装例です。

```csharp
using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

public class PriorityMatrixPicker : Control
{
    private Image _baseLayer;
    private Image[] _glowMasks = new Image[49];
    private byte[] _hitMap; // 282 x 340 bytes
    private const int MapWidth = 282;
    private const int MapHeight = 340;

    private int _selectedRank = 1;  // クリックで確定したRank (1〜49)
    private int _hoveredRank = 0;   // 現在カーソルが乗っているRank (0: なし)

    public event EventHandler<int> SelectedRankChanged;

    public PriorityMatrixPicker()
    {
        this.DoubleBuffered = true;
        this.Size = new Size(MapWidth, MapHeight);

        // 1. 原画の読み込み
        _baseLayer = Image.FromFile(@"ui_parts\base_layer.png");

        // 2. マスク画像の読み込み
        for (int i = 1; i <= 49; i++)
        {
            _glowMasks[i - 1] = Image.FromFile($@"ui_parts\glow_masks\mask_rank_{i:D2}.png");
        }

        // 3. 当たり判定テーブルの読み込み (93 KB)
        _hitMap = File.ReadAllBytes(@"ui_parts\hit_map.bin");
    }

    public int SelectedRank
    {
        get => _selectedRank;
        set
        {
            if (_selectedRank != value && value >= 1 && value <= 49)
            {
                _selectedRank = value;
                this.Invalidate();
                SelectedRankChanged?.Invoke(this, _selectedRank);
            }
        }
    }

    // マウス移動: O(1) で即座にホバー判定
    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);

        int rankAtCursor = 0;
        if (e.X >= 0 && e.X < MapWidth && e.Y >= 0 && e.Y < MapHeight)
        {
            rankAtCursor = _hitMap[e.Y * MapWidth + e.X];
        }

        if (_hoveredRank != rankAtCursor)
        {
            _hoveredRank = rankAtCursor;
            this.Cursor = (_hoveredRank > 0) ? Cursors.Hand : Cursors.Default;
            this.Invalidate(); // ホバー表示を更新
        }
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        if (_hoveredRank != 0)
        {
            _hoveredRank = 0;
            this.Invalidate();
        }
    }

    // クリック: 選択確定
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Button == MouseButtons.Left && _hoveredRank > 0)
        {
            this.SelectedRank = _hoveredRank;
        }
    }

    // 描画: ベース原画の上に、ホバー（優先）または選択中のマスクを重ねるだけ
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;

        // 1. ベース原画 (45%輝度)
        g.DrawImage(_baseLayer, 0, 0, MapWidth, MapHeight);

        // 2. ホバー中ならホバー柱のマスク、そうでなければ選択中柱のマスクを重ねる
        int activeRank = (_hoveredRank > 0) ? _hoveredRank : _selectedRank;
        if (activeRank >= 1 && activeRank <= 49)
        {
            g.DrawImage(_glowMasks[activeRank - 1], 0, 0, MapWidth, MapHeight);
        }
    }
}
```
