using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace EcoQoSMonitor
{
    static class Program
    {
        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool SetProcessDpiAwarenessContext(IntPtr dpiContext);
        private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (IntPtr)(-4);

        [STAThread]
        static void Main()
        {
            try
            {
                SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
            catch { }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }

    public class MainForm : Form
    {
        [DllImport("user32.dll")]
        public static extern uint GetDpiForWindow(IntPtr hWnd);

        [StructLayout(LayoutKind.Sequential)]
        public struct POWER_THROTTLING_STATE
        {
            public uint Version;
            public uint ControlMask;
            public uint StateMask;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(uint processAccess, bool bInheritHandle, int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetProcessInformation(
            IntPtr hProcess,
            int ProcessInformationClass,
            ref POWER_THROTTLING_STATE ProcessInformation,
            uint ProcessInformationSize
        );

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool SetProcessInformation(
            IntPtr hProcess,
            int ProcessInformationClass,
            ref POWER_THROTTLING_STATE ProcessInformation,
            uint ProcessInformationSize
        );

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenThread(uint threadAccess, bool bInheritHandle, int threadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetThreadInformation(
            IntPtr hThread,
            int ThreadInformationClass,
            ref POWER_THROTTLING_STATE ThreadInformation,
            uint ThreadInformationSize
        );

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool SetThreadInformation(
            IntPtr hThread,
            int ThreadInformationClass,
            ref POWER_THROTTLING_STATE ThreadInformation,
            uint ThreadInformationSize
        );

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetProcessAffinityMask(
            IntPtr hProcess,
            out UIntPtr lpProcessAffinityMask,
            out UIntPtr lpSystemAffinityMask
        );

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);

        const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        const uint PROCESS_SET_INFORMATION = 0x0200;
        const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
        const uint THREAD_SET_INFORMATION = 0x0020;

        const int ProcessPowerThrottling = 4;
        const int ThreadPowerThrottling = 3;

        const uint POWER_THROTTLING_EXECUTION_SPEED = 0x1;
        const uint POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 0x4;

        private Panel topPanel;
        private Label lblInterval;
        private NumericUpDown numInterval;
        private CheckBox chkTopMost;
        private Label lblF;
        private ComboBox cmbFilter;
        private Button btnClearAll;
        private Label lblStatus;
        private SplitContainer split;
        private ListView lvProcesses;
        private ListView lvThreads;
        private Label lblProcCount;
        private Label lblThreadCount;

        private System.Windows.Forms.Timer scanTimer;
        private volatile bool isScanning = false;
        private volatile bool isClearing = false;

        private ScanResult lastScanResult;

        // Sort management
        private int procSortCol = 0;
        private bool procSortAsc = true;
        private int threadSortCol = 0;
        private bool threadSortAsc = true;

        private readonly string[] procColNames = new string[] {
            "PID", "CPU#", "Process Name", "QoS Type", "Speed Limit (0x1)", "Ignore Timer (0x4)", "Ctrl", "State"
        };
        private readonly int[] procBaseColWidths = new int[] {
            70, 75, 150, 90, 125, 135, 55, 55
        };

        private readonly string[] threadColNames = new string[] {
            "PID", "TID", "Process Name", "QoS Type", "Speed Limit (0x1)", "Scope", "Ctrl", "State"
        };
        private readonly int[] threadBaseColWidths = new int[] {
            65, 70, 140, 90, 125, 95, 55, 55
        };

        public MainForm()
        {
            InitializeComponent();
        }

        private static void SetDoubleBuffered(Control control)
        {
            typeof(Control).InvokeMember("DoubleBuffered",
                BindingFlags.SetProperty | BindingFlags.Instance | BindingFlags.NonPublic,
                null, control, new object[] { true });
        }

        private float GetDpiScale()
        {
            try
            {
                if (this.IsHandleCreated)
                {
                    uint dpi = GetDpiForWindow(this.Handle);
                    if (dpi > 0) return dpi / 96.0f;
                }
            }
            catch { }

            using (Graphics g = Graphics.FromHwnd(IntPtr.Zero))
            {
                return g.DpiX / 96.0f;
            }
        }

        private void InitializeComponent()
        {
            this.Text = "QoS (EcoQoS / HighQoS) Real-time Monitor (All PID / TID)";
            this.Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            this.StartPosition = FormStartPosition.CenterScreen;

            float initScale = GetDpiScale();
            if (initScale <= 0) initScale = 1.0f;
            this.ClientSize = new Size((int)(1495 * initScale), (int)(740 * initScale));
            this.MinimumSize = new Size((int)(950 * initScale), (int)(500 * initScale));

            // Top Panel
            topPanel = new Panel
            {
                Dock = DockStyle.Top,
                BackColor = Color.FromArgb(245, 247, 250)
            };

            lblInterval = new Label
            {
                Text = "Interval (ms):",
                AutoSize = true
            };

            numInterval = new NumericUpDown
            {
                Minimum = 100,
                Maximum = 60000,
                Value = 1000,
                Increment = 100
            };
            numInterval.ValueChanged += (s, e) =>
            {
                if (scanTimer != null) scanTimer.Interval = (int)numInterval.Value;
            };

            chkTopMost = new CheckBox
            {
                Text = "TopMost",
                AutoSize = true,
                Checked = false
            };
            chkTopMost.CheckedChanged += (s, e) => { this.TopMost = chkTopMost.Checked; };

            lblF = new Label
            {
                Text = "Filter:",
                AutoSize = true
            };

            cmbFilter = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            cmbFilter.Items.AddRange(new object[] {
                "Configured QoS (High & Eco)",
                "EcoQoS only",
                "HighQoS only",
                "None: all PID / TID"
            });
            cmbFilter.SelectedIndex = 0;
            cmbFilter.SelectedIndexChanged += (s, e) => { TriggerScan(); };

            btnClearAll = new Button
            {
                Text = "Clear EcoQoS (All)",
                BackColor = Color.FromArgb(254, 238, 238),
                FlatStyle = FlatStyle.System
            };
            btnClearAll.Click += BtnClearAll_Click;

            lblStatus = new Label
            {
                Text = "Initializing...",
                AutoSize = true,
                ForeColor = Color.FromArgb(40, 40, 40)
            };

            topPanel.Controls.Add(lblInterval);
            topPanel.Controls.Add(numInterval);
            topPanel.Controls.Add(chkTopMost);
            topPanel.Controls.Add(lblF);
            topPanel.Controls.Add(cmbFilter);
            topPanel.Controls.Add(btnClearAll);
            topPanel.Controls.Add(lblStatus);

            // Split Container
            split = new SplitContainer
            {
                Dock = DockStyle.Fill,
                Orientation = Orientation.Vertical,
                SplitterWidth = 6
            };

            // Left Pane (PID List)
            Panel leftHeader = new Panel
            {
                Dock = DockStyle.Top,
                BackColor = Color.FromArgb(235, 240, 245),
                Padding = new Padding(8, 6, 8, 4)
            };
            lblProcCount = new Label
            {
                Text = "Process QoS List (0 items)",
                Dock = DockStyle.Fill,
                Font = new Font("Segoe UI", 9F, FontStyle.Bold, GraphicsUnit.Point)
            };
            leftHeader.Controls.Add(lblProcCount);

            lvProcesses = new ListView
            {
                Dock = DockStyle.Fill,
                View = View.Details,
                FullRowSelect = true,
                GridLines = true,
                Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point)
            };
            SetDoubleBuffered(lvProcesses);
            for (int i = 0; i < procColNames.Length; i++)
            {
                HorizontalAlignment align = (i == 0) ? HorizontalAlignment.Right :
                    (i == 1 ? HorizontalAlignment.Center :
                    (i == 2 ? HorizontalAlignment.Left : HorizontalAlignment.Center));
                lvProcesses.Columns.Add(procColNames[i], (int)(procBaseColWidths[i] * initScale), align);
            }
            lvProcesses.ColumnClick += LvProcesses_ColumnClick;

            split.Panel1.Controls.Add(lvProcesses);
            split.Panel1.Controls.Add(leftHeader);

            // Right Pane (TID List)
            Panel rightHeader = new Panel
            {
                Dock = DockStyle.Top,
                BackColor = Color.FromArgb(235, 240, 245),
                Padding = new Padding(8, 6, 8, 4)
            };
            lblThreadCount = new Label
            {
                Text = "Thread QoS List (0 items)",
                Dock = DockStyle.Fill,
                Font = new Font("Segoe UI", 9F, FontStyle.Bold, GraphicsUnit.Point)
            };
            rightHeader.Controls.Add(lblThreadCount);

            lvThreads = new ListView
            {
                Dock = DockStyle.Fill,
                View = View.Details,
                FullRowSelect = true,
                GridLines = true,
                Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point)
            };
            SetDoubleBuffered(lvThreads);
            for (int i = 0; i < threadColNames.Length; i++)
            {
                HorizontalAlignment align = (i <= 1) ? HorizontalAlignment.Right :
                    (i == 2 ? HorizontalAlignment.Left : HorizontalAlignment.Center);
                lvThreads.Columns.Add(threadColNames[i], (int)(threadBaseColWidths[i] * initScale), align);
            }
            lvThreads.ColumnClick += LvThreads_ColumnClick;

            split.Panel2.Controls.Add(lvThreads);
            split.Panel2.Controls.Add(rightHeader);

            this.Controls.Add(split);
            this.Controls.Add(topPanel);

            UpdateHeaderSortArrows();

            scanTimer = new System.Windows.Forms.Timer();
            scanTimer.Interval = (int)numInterval.Value;
            scanTimer.Tick += (s, e) => TriggerScan();
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            ApplyDpiScaling();
            StartScanning();
        }

        private void ApplyDpiScaling()
        {
            float scale = GetDpiScale();
            if (scale <= 0) scale = 1.0f;

            // Apply exact DPI scaling
            int targetClientW = (int)(1495 * scale);
            int targetClientH = (int)(740 * scale);
            this.ClientSize = new Size(targetClientW, targetClientH);
            this.MinimumSize = new Size((int)(950 * scale), (int)(500 * scale));

            // Splitter position: left pane accommodates new CPU# column
            split.SplitterDistance = (int)(770 * scale);

            // Apply column width DPI scale
            for (int i = 0; i < lvProcesses.Columns.Count; i++)
            {
                lvProcesses.Columns[i].Width = (int)(procBaseColWidths[i] * scale);
            }
            for (int i = 0; i < lvThreads.Columns.Count; i++)
            {
                lvThreads.Columns[i].Width = (int)(threadBaseColWidths[i] * scale);
            }

            // Top Panel layout and dimensions
            topPanel.Height = (int)(46 * scale);
            numInterval.Width = (int)(65 * scale);
            cmbFilter.Width = (int)(215 * scale);
            btnClearAll.Size = new Size((int)(150 * scale), (int)(28 * scale));

            lblInterval.Location = new Point((int)(10 * scale), (int)(14 * scale));
            numInterval.Location = new Point((int)(92 * scale), (int)(11 * scale));
            chkTopMost.Location = new Point((int)(165 * scale), (int)(13 * scale));
            lblF.Location = new Point((int)(245 * scale), (int)(14 * scale));
            cmbFilter.Location = new Point((int)(285 * scale), (int)(11 * scale));
            btnClearAll.Location = new Point((int)(510 * scale), (int)(9 * scale));
            lblStatus.Location = new Point((int)(670 * scale), (int)(14 * scale));
        }

        private void UpdateHeaderSortArrows()
        {
            for (int i = 0; i < lvProcesses.Columns.Count; i++)
            {
                string baseName = procColNames[i];
                if (i == procSortCol)
                    lvProcesses.Columns[i].Text = baseName + (procSortAsc ? " ▲" : " ▼");
                else
                    lvProcesses.Columns[i].Text = baseName;
            }
            for (int i = 0; i < lvThreads.Columns.Count; i++)
            {
                string baseName = threadColNames[i];
                if (i == threadSortCol)
                    lvThreads.Columns[i].Text = baseName + (threadSortAsc ? " ▲" : " ▼");
                else
                    lvThreads.Columns[i].Text = baseName;
            }
        }

        private void LvProcesses_ColumnClick(object sender, ColumnClickEventArgs e)
        {
            if (procSortCol == e.Column)
            {
                procSortAsc = !procSortAsc;
            }
            else
            {
                procSortCol = e.Column;
                procSortAsc = true;
            }
            UpdateHeaderSortArrows();

            // Immediate sort reflection (0ms)
            if (lastScanResult != null && lastScanResult.ProcRows != null)
            {
                SortProcRows(lastScanResult.ProcRows);
                UpdateListViewProcesses(lastScanResult.ProcRows);
                if (lvProcesses.Items.Count > 0)
                    lvProcesses.TopItem = lvProcesses.Items[0];
            }
        }

        private void LvThreads_ColumnClick(object sender, ColumnClickEventArgs e)
        {
            if (threadSortCol == e.Column)
            {
                threadSortAsc = !threadSortAsc;
            }
            else
            {
                threadSortCol = e.Column;
                threadSortAsc = true;
            }
            UpdateHeaderSortArrows();

            // Immediate sort reflection (0ms)
            if (lastScanResult != null && lastScanResult.ThreadRows != null)
            {
                SortThreadRows(lastScanResult.ThreadRows);
                UpdateListViewThreads(lastScanResult.ThreadRows);
                if (lvThreads.Items.Count > 0)
                    lvThreads.TopItem = lvThreads.Items[0];
            }
        }

        private void StartScanning()
        {
            scanTimer.Start();
            TriggerScan();
        }

        private void TriggerScan()
        {
            if (isScanning || isClearing) return;
            isScanning = true;

            int filterMode = 0;
            if (!this.IsDisposed && this.IsHandleCreated)
            {
                filterMode = cmbFilter.SelectedIndex;
            }

            ThreadPool.QueueUserWorkItem(state =>
            {
                PerformScan(filterMode);
            });
        }

        public class ProcRow
        {
            public string Pid;
            public string Cpu;
            public ulong AffinityMask;
            public string Name;
            public string QosType;
            public string SpeedThrottling;
            public string TimerIgnore;
            public string Ctrl;
            public string State;
            public Color TextColor;
        }

        public class ThreadRow
        {
            public string Pid;
            public string Tid;
            public string Name;
            public string QosType;
            public string SpeedThrottling;
            public string AssignType;
            public string Ctrl;
            public string State;
            public Color TextColor;
        }

        private class ScanResult
        {
            public List<ProcRow> ProcRows = new List<ProcRow>();
            public List<ThreadRow> ThreadRows = new List<ThreadRow>();
            public int TotalProcs;
            public int TotalThreads;
            public long ElapsedMs;
        }

        private void PerformScan(int filterMode)
        {
            Stopwatch sw = Stopwatch.StartNew();
            ScanResult result = new ScanResult();

            Process[] processes = Process.GetProcesses();
            result.TotalProcs = processes.Length;

            uint stateSize = (uint)Marshal.SizeOf(typeof(POWER_THROTTLING_STATE));

            foreach (Process p in processes)
            {
                bool hasProcQos = false;
                bool isProcEco = false;
                bool isProcHigh = false;
                POWER_THROTTLING_STATE pState = new POWER_THROTTLING_STATE { Version = 1 };

                IntPtr hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, p.Id);
                ulong affMask = 0;
                ulong sysMask = 0;
                if (hProc != IntPtr.Zero)
                {
                    UIntPtr pAff, sAff;
                    if (GetProcessAffinityMask(hProc, out pAff, out sAff))
                    {
                        affMask = (ulong)pAff.ToUInt64();
                        sysMask = (ulong)sAff.ToUInt64();
                    }
                    else
                    {
                        try { affMask = (ulong)p.ProcessorAffinity.ToInt64(); } catch { }
                    }

                    if (GetProcessInformation(hProc, ProcessPowerThrottling, ref pState, stateSize))
                    {
                        if ((pState.ControlMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                        {
                            hasProcQos = true;
                            if ((pState.StateMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                                isProcEco = true;
                            else
                                isProcHigh = true;
                        }
                        else if ((pState.ControlMask & POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) != 0)
                        {
                            hasProcQos = true;
                        }
                    }
                    CloseHandle(hProc);
                }
                else
                {
                    try { affMask = (ulong)p.ProcessorAffinity.ToInt64(); } catch { }
                }

                bool includeProc = false;
                if (filterMode == 0)
                    includeProc = hasProcQos;
                else if (filterMode == 1)
                    includeProc = isProcEco;
                else if (filterMode == 2)
                    includeProc = isProcHigh;
                else if (filterMode == 3)
                    includeProc = true;

                if (includeProc)
                {
                    string qosStr = isProcEco ? "EcoQoS" : (isProcHigh ? "HighQoS" : "Default");
                    string speedStr = "-";
                    if ((pState.ControlMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                    {
                        speedStr = ((pState.StateMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                            ? "Enabled (Eco)" : "Disabled (High)";
                    }

                    string timerStr = "-";
                    if ((pState.ControlMask & POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) != 0)
                    {
                        timerStr = ((pState.StateMask & POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) != 0)
                            ? "Ignore ON" : "Preserve Timer";
                    }

                    Color rowColor = isProcHigh ? Color.FromArgb(0, 102, 204) : (isProcEco ? Color.FromArgb(178, 34, 34) : Color.Black);

                    result.ProcRows.Add(new ProcRow
                    {
                        Pid = p.Id.ToString(),
                        Cpu = FormatAffinityMask(affMask, sysMask),
                        AffinityMask = affMask,
                        Name = p.ProcessName,
                        QosType = qosStr,
                        SpeedThrottling = speedStr,
                        TimerIgnore = timerStr,
                        Ctrl = string.Format("0x{0:X}", pState.ControlMask),
                        State = string.Format("0x{0:X}", pState.StateMask),
                        TextColor = rowColor
                    });
                }

                try
                {
                    foreach (ProcessThread th in p.Threads)
                    {
                        result.TotalThreads++;
                        IntPtr hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, th.Id);
                        if (hThread != IntPtr.Zero)
                        {
                            POWER_THROTTLING_STATE thState = new POWER_THROTTLING_STATE { Version = 1 };
                            bool thHasQos = false;
                            bool thEco = false;
                            bool thHigh = false;
                            bool isExplicit = false;

                            if (GetThreadInformation(hThread, ThreadPowerThrottling, ref thState, stateSize))
                            {
                                if ((thState.ControlMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                                {
                                    thHasQos = true;
                                    isExplicit = true;
                                    if ((thState.StateMask & POWER_THROTTLING_EXECUTION_SPEED) != 0)
                                        thEco = true;
                                    else
                                        thHigh = true;
                                }
                                else
                                {
                                    thEco = isProcEco;
                                    thHigh = isProcHigh;
                                    thHasQos = hasProcQos;
                                }
                            }
                            else
                            {
                                thEco = isProcEco;
                                thHigh = isProcHigh;
                                thHasQos = hasProcQos;
                            }

                            bool includeThread = false;
                            if (filterMode == 0)
                                includeThread = thHasQos;
                            else if (filterMode == 1)
                                includeThread = thEco;
                            else if (filterMode == 2)
                                includeThread = thHigh;
                            else if (filterMode == 3)
                                includeThread = true;

                            if (includeThread)
                            {
                                string thQosStr = thEco ? "EcoQoS" : (thHigh ? "HighQoS" : "Default");
                                string thSpeedStr = "-";
                                if (isExplicit)
                                {
                                    thSpeedStr = thEco ? "Enabled (Eco)" : "Disabled (High)";
                                }
                                else if (isProcEco)
                                {
                                    thSpeedStr = "Inherited (Eco)";
                                }
                                else if (isProcHigh)
                                {
                                    thSpeedStr = "Inherited (High)";
                                }

                                Color thColor = thHigh ? Color.FromArgb(0, 102, 204) : (thEco ? Color.FromArgb(178, 34, 34) : Color.Black);

                                result.ThreadRows.Add(new ThreadRow
                                {
                                    Pid = p.Id.ToString(),
                                    Tid = th.Id.ToString(),
                                    Name = p.ProcessName,
                                    QosType = thQosStr,
                                    SpeedThrottling = thSpeedStr,
                                    AssignType = isExplicit ? "[Explicit]" : "[Inherited]",
                                    Ctrl = string.Format("0x{0:X}", thState.ControlMask),
                                    State = string.Format("0x{0:X}", thState.StateMask),
                                    TextColor = thColor
                                });
                            }

                            CloseHandle(hThread);
                        }
                    }
                }
                catch
                {
                }
            }

            sw.Stop();
            result.ElapsedMs = sw.ElapsedMilliseconds;

            if (!this.IsDisposed && this.IsHandleCreated)
            {
                this.BeginInvoke(new Action(() =>
                {
                    lastScanResult = result;
                    UpdateUI(result);
                    isScanning = false;
                }));
            }
            else
            {
                isScanning = false;
            }
        }

        private static string FormatAffinityMask(ulong mask, ulong sysMask)
        {
            if (mask == 0) return "-";
            if (sysMask != 0 && mask == sysMask) return "All";

            // 単一コア判定 (power of 2)
            if ((mask & (mask - 1)) == 0)
            {
                for (int i = 0; i < 64; i++)
                {
                    if ((mask & (1UL << i)) != 0)
                        return i.ToString();
                }
            }

            // 連続・非連続のレンジ集約 (例: "0-1, 4-9")
            List<string> ranges = new List<string>();
            int start = -1;
            int prev = -1;

            for (int i = 0; i < 64; i++)
            {
                bool isSet = (mask & (1UL << i)) != 0;
                if (isSet)
                {
                    if (start == -1)
                    {
                        start = i;
                        prev = i;
                    }
                    else if (i == prev + 1)
                    {
                        prev = i;
                    }
                    else
                    {
                        ranges.Add(start == prev ? start.ToString() : string.Format("{0}-{1}", start, prev));
                        start = i;
                        prev = i;
                    }
                }
            }
            if (start != -1)
            {
                ranges.Add(start == prev ? start.ToString() : string.Format("{0}-{1}", start, prev));
            }

            string result = string.Join(", ", ranges.ToArray());
            if (result.Length > 16)
            {
                return string.Format("0x{0:X}", mask);
            }
            return result;
        }

        private void SortProcRows(List<ProcRow> rows)
        {
            rows.Sort((a, b) =>
            {
                int cmp = 0;
                switch (procSortCol)
                {
                    case 0: // PID
                        long pidA = 0, pidB = 0;
                        long.TryParse(a.Pid, out pidA);
                        long.TryParse(b.Pid, out pidB);
                        cmp = pidA.CompareTo(pidB);
                        break;
                    case 1: // CPU#
                        cmp = a.AffinityMask.CompareTo(b.AffinityMask);
                        if (cmp == 0)
                        {
                            long pa = 0, pb = 0;
                            long.TryParse(a.Pid, out pa);
                            long.TryParse(b.Pid, out pb);
                            cmp = pa.CompareTo(pb);
                        }
                        break;
                    case 2: // Process Name
                        cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                        {
                            long pa = 0, pb = 0;
                            long.TryParse(a.Pid, out pa);
                            long.TryParse(b.Pid, out pb);
                            cmp = pa.CompareTo(pb);
                        }
                        break;
                    case 3: // QoS Type
                        cmp = string.Compare(a.QosType, b.QosType, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 4: // Speed Limit
                        cmp = string.Compare(a.SpeedThrottling, b.SpeedThrottling, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 5: // Ignore Timer
                        cmp = string.Compare(a.TimerIgnore, b.TimerIgnore, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 6: // Ctrl
                        uint cA = 0, cB = 0;
                        uint.TryParse(a.Ctrl.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out cA);
                        uint.TryParse(b.Ctrl.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out cB);
                        cmp = cA.CompareTo(cB);
                        break;
                    case 7: // State
                        uint sA = 0, sB = 0;
                        uint.TryParse(a.State.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out sA);
                        uint.TryParse(b.State.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out sB);
                        cmp = sA.CompareTo(sB);
                        break;
                }
                return procSortAsc ? cmp : -cmp;
            });
        }

        private void SortThreadRows(List<ThreadRow> rows)
        {
            rows.Sort((a, b) =>
            {
                int cmp = 0;
                switch (threadSortCol)
                {
                    case 0: // PID
                        long pA = 0, pB = 0;
                        long.TryParse(a.Pid, out pA);
                        long.TryParse(b.Pid, out pB);
                        cmp = pA.CompareTo(pB);
                        if (cmp == 0)
                        {
                            long tA = 0, tB = 0;
                            long.TryParse(a.Tid, out tA);
                            long.TryParse(b.Tid, out tB);
                            cmp = tA.CompareTo(tB);
                        }
                        break;
                    case 1: // TID
                        long tidA = 0, tidB = 0;
                        long.TryParse(a.Tid, out tidA);
                        long.TryParse(b.Tid, out tidB);
                        cmp = tidA.CompareTo(tidB);
                        break;
                    case 2: // Process Name
                        cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                        {
                            long pa = 0, pb = 0;
                            long.TryParse(a.Pid, out pa);
                            long.TryParse(b.Pid, out pb);
                            cmp = pa.CompareTo(pb);
                            if (cmp == 0)
                            {
                                long ta = 0, tb = 0;
                                long.TryParse(a.Tid, out ta);
                                long.TryParse(b.Tid, out tb);
                                cmp = ta.CompareTo(tb);
                            }
                        }
                        break;
                    case 3: // QoS Type
                        cmp = string.Compare(a.QosType, b.QosType, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 4: // Speed Limit
                        cmp = string.Compare(a.SpeedThrottling, b.SpeedThrottling, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 5: // Scope
                        cmp = string.Compare(a.AssignType, b.AssignType, StringComparison.OrdinalIgnoreCase);
                        if (cmp == 0)
                            cmp = string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                        break;
                    case 6: // Ctrl
                        uint cA = 0, cB = 0;
                        uint.TryParse(a.Ctrl.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out cA);
                        uint.TryParse(b.Ctrl.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out cB);
                        cmp = cA.CompareTo(cB);
                        break;
                    case 7: // State
                        uint sA = 0, sB = 0;
                        uint.TryParse(a.State.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out sA);
                        uint.TryParse(b.State.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out sB);
                        cmp = sA.CompareTo(sB);
                        break;
                }
                return threadSortAsc ? cmp : -cmp;
            });
        }

        private void UpdateUI(ScanResult result)
        {
            if (!isClearing)
            {
                lblStatus.Text = string.Format("Scan: {0} ms | All PID: {1} | All TID: {2} | Updated: {3:HH:mm:ss}",
                    result.ElapsedMs, result.TotalProcs, result.TotalThreads, DateTime.Now);
            }

            lblProcCount.Text = string.Format("Process QoS List ({0} items)", result.ProcRows.Count);
            lblThreadCount.Text = string.Format("Thread QoS List ({0} items)", result.ThreadRows.Count);

            SortProcRows(result.ProcRows);
            SortThreadRows(result.ThreadRows);

            UpdateListViewProcesses(result.ProcRows);
            UpdateListViewThreads(result.ThreadRows);
        }

        private void UpdateListViewProcesses(List<ProcRow> rows)
        {
            if (lvProcesses.Items.Count == rows.Count)
            {
                bool identical = true;
                for (int i = 0; i < rows.Count; i++)
                {
                    ListViewItem it = lvProcesses.Items[i];
                    ProcRow r = rows[i];
                    if (it.Text != r.Pid ||
                        it.SubItems[1].Text != r.Cpu ||
                        it.SubItems[2].Text != r.Name ||
                        it.SubItems[3].Text != r.QosType ||
                        it.SubItems[4].Text != r.SpeedThrottling ||
                        it.SubItems[5].Text != r.TimerIgnore ||
                        it.SubItems[6].Text != r.Ctrl ||
                        it.SubItems[7].Text != r.State ||
                        it.ForeColor != r.TextColor)
                    {
                        identical = false;
                        break;
                    }
                }
                if (identical) return;
            }

            lvProcesses.BeginUpdate();
            try
            {
                while (lvProcesses.Items.Count > rows.Count)
                {
                    lvProcesses.Items.RemoveAt(lvProcesses.Items.Count - 1);
                }

                while (lvProcesses.Items.Count < rows.Count)
                {
                    ListViewItem item = new ListViewItem("");
                    for (int s = 0; s < 7; s++) item.SubItems.Add("");
                    lvProcesses.Items.Add(item);
                }

                for (int i = 0; i < rows.Count; i++)
                {
                    ListViewItem it = lvProcesses.Items[i];
                    ProcRow r = rows[i];
                    if (it.Text != r.Pid) it.Text = r.Pid;
                    if (it.SubItems[1].Text != r.Cpu) it.SubItems[1].Text = r.Cpu;
                    if (it.SubItems[2].Text != r.Name) it.SubItems[2].Text = r.Name;
                    if (it.SubItems[3].Text != r.QosType) it.SubItems[3].Text = r.QosType;
                    if (it.SubItems[4].Text != r.SpeedThrottling) it.SubItems[4].Text = r.SpeedThrottling;
                    if (it.SubItems[5].Text != r.TimerIgnore) it.SubItems[5].Text = r.TimerIgnore;
                    if (it.SubItems[6].Text != r.Ctrl) it.SubItems[6].Text = r.Ctrl;
                    if (it.SubItems[7].Text != r.State) it.SubItems[7].Text = r.State;
                    if (it.ForeColor != r.TextColor) it.ForeColor = r.TextColor;
                }
            }
            finally
            {
                lvProcesses.EndUpdate();
            }
        }

        private void UpdateListViewThreads(List<ThreadRow> rows)
        {
            if (lvThreads.Items.Count == rows.Count)
            {
                bool identical = true;
                for (int i = 0; i < rows.Count; i++)
                {
                    ListViewItem it = lvThreads.Items[i];
                    ThreadRow r = rows[i];
                    if (it.Text != r.Pid ||
                        it.SubItems[1].Text != r.Tid ||
                        it.SubItems[2].Text != r.Name ||
                        it.SubItems[3].Text != r.QosType ||
                        it.SubItems[4].Text != r.SpeedThrottling ||
                        it.SubItems[5].Text != r.AssignType ||
                        it.SubItems[6].Text != r.Ctrl ||
                        it.SubItems[7].Text != r.State ||
                        it.ForeColor != r.TextColor)
                    {
                        identical = false;
                        break;
                    }
                }
                if (identical) return;
            }

            lvThreads.BeginUpdate();
            try
            {
                while (lvThreads.Items.Count > rows.Count)
                {
                    lvThreads.Items.RemoveAt(lvThreads.Items.Count - 1);
                }

                while (lvThreads.Items.Count < rows.Count)
                {
                    ListViewItem item = new ListViewItem("");
                    for (int s = 0; s < 7; s++) item.SubItems.Add("");
                    lvThreads.Items.Add(item);
                }

                for (int i = 0; i < rows.Count; i++)
                {
                    ListViewItem it = lvThreads.Items[i];
                    ThreadRow r = rows[i];
                    if (it.Text != r.Pid) it.Text = r.Pid;
                    if (it.SubItems[1].Text != r.Tid) it.SubItems[1].Text = r.Tid;
                    if (it.SubItems[2].Text != r.Name) it.SubItems[2].Text = r.Name;
                    if (it.SubItems[3].Text != r.QosType) it.SubItems[3].Text = r.QosType;
                    if (it.SubItems[4].Text != r.SpeedThrottling) it.SubItems[4].Text = r.SpeedThrottling;
                    if (it.SubItems[5].Text != r.AssignType) it.SubItems[5].Text = r.AssignType;
                    if (it.SubItems[6].Text != r.Ctrl) it.SubItems[6].Text = r.Ctrl;
                    if (it.SubItems[7].Text != r.State) it.SubItems[7].Text = r.State;
                    if (it.ForeColor != r.TextColor) it.ForeColor = r.TextColor;
                }
            }
            finally
            {
                lvThreads.EndUpdate();
            }
        }

        private void BtnClearAll_Click(object sender, EventArgs e)
        {
            if (isClearing) return;
            isClearing = true;
            btnClearAll.Enabled = false;
            lblStatus.Text = "Clearing EcoQoS flags for all PID/TID...";

            ThreadPool.QueueUserWorkItem(state =>
            {
                int unthrottledProcs = 0;
                int unthrottledThreads = 0;

                Process[] processes = Process.GetProcesses();
                uint stateSize = (uint)Marshal.SizeOf(typeof(POWER_THROTTLING_STATE));

                POWER_THROTTLING_STATE pStateOff = new POWER_THROTTLING_STATE
                {
                    Version = 1,
                    ControlMask = POWER_THROTTLING_EXECUTION_SPEED | POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                    StateMask = 0
                };

                POWER_THROTTLING_STATE tStateOff = new POWER_THROTTLING_STATE
                {
                    Version = 1,
                    ControlMask = POWER_THROTTLING_EXECUTION_SPEED,
                    StateMask = 0
                };

                foreach (Process p in processes)
                {
                    IntPtr hProc = OpenProcess(PROCESS_SET_INFORMATION, false, p.Id);
                    if (hProc != IntPtr.Zero)
                    {
                        if (SetProcessInformation(hProc, ProcessPowerThrottling, ref pStateOff, stateSize))
                        {
                            unthrottledProcs++;
                        }
                        CloseHandle(hProc);
                    }

                    try
                    {
                        foreach (ProcessThread th in p.Threads)
                        {
                            IntPtr hThread = OpenThread(THREAD_SET_INFORMATION, false, th.Id);
                            if (hThread != IntPtr.Zero)
                            {
                                if (SetThreadInformation(hThread, ThreadPowerThrottling, ref tStateOff, stateSize))
                                {
                                    unthrottledThreads++;
                                }
                                CloseHandle(hThread);
                            }
                        }
                    }
                    catch
                    {
                    }
                }

                if (!this.IsDisposed && this.IsHandleCreated)
                {
                    this.BeginInvoke(new Action(() =>
                    {
                        isClearing = false;
                        btnClearAll.Enabled = true;
                        lblStatus.Text = string.Format("Cleared: {0} PID / {1} TID unthrottled | {2:HH:mm:ss}",
                            unthrottledProcs, unthrottledThreads, DateTime.Now);
                        TriggerScan();
                    }));
                }
                else
                {
                    isClearing = false;
                }
            });
        }
    }
}
