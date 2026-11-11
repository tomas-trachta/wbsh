// Runs one command inside a Windows Job object and measures it.
//
// The job is what makes the numbers honest: CPU time, peak memory and the
// process count cover the whole tree (wbsh plus every child it spawns), and
// a timeout kills the whole tree, not only the root process. The root starts
// suspended so no child can escape the job before it is assigned.
//
// Compiled at run time by stress.ps1 through Add-Type.

using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace WbshStress
{
	public sealed class ProbeSample
	{
		public double Ms;
		public long PrivateBytes;
		public long WorkingSet;
		public int Handles;
	}

	public sealed class ProbeResult
	{
		public long ExitCode;
		public bool TimedOut;
		public bool Crashed;
		public string CrashName = "";
		public double WallMs;
		public double CpuMs;
		public long PeakJobMemory;
		public long PeakProcessMemory;
		public long RootPeakWorkingSet;
		public long RootPeakPrivate;
		public int PeakHandles;
		public int TotalProcesses;
		public int LeftoverProcesses;
		public List<ProbeSample> Samples = new List<ProbeSample>();
	}

	public sealed class ProbeOptions
	{
		public string CommandLine = "";
		public string WorkDir = null;
		public string StdinPath = null;
		public string StdoutPath = null;
		public string StderrPath = null;
		public int TimeoutMs = 60000;
		public int SampleMs = 20;
		public bool KeepSamples = false;
		public long MemoryLimitBytes = 0;
		public IDictionary Environment = null;
	}

	public static class ProcessProbe
	{
		const uint CREATE_SUSPENDED = 0x00000004;
		const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
		const uint CREATE_NO_WINDOW = 0x08000000;
		const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
		const int STARTF_USESTDHANDLES = 0x00000100;
		const uint GENERIC_READ = 0x80000000;
		const uint GENERIC_WRITE = 0x40000000;
		const uint FILE_SHARE_READ = 0x1;
		const uint FILE_SHARE_WRITE = 0x2;
		const uint OPEN_EXISTING = 3;
		const uint CREATE_ALWAYS = 2;
		const uint WAIT_TIMEOUT = 0x102;
		const int JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;
		const int JOB_OBJECT_LIMIT_JOB_MEMORY = 0x200;
		const int JobObjectBasicAccountingInformation = 1;
		const int JobObjectBasicProcessIdList = 3;
		const int JobObjectExtendedLimitInformation = 9;
		static readonly IntPtr PROC_THREAD_ATTRIBUTE_HANDLE_LIST = (IntPtr)0x20002;

		[StructLayout(LayoutKind.Sequential)]
		struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public bool bInheritHandle; }

		[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
		struct STARTUPINFO
		{
			public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
			public int dwX; public int dwY; public int dwXSize; public int dwYSize;
			public int dwXCountChars; public int dwYCountChars; public int dwFillAttribute; public int dwFlags;
			public short wShowWindow; public short cbReserved2; public IntPtr lpReserved2;
			public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }

		[StructLayout(LayoutKind.Sequential)]
		struct PROCESS_INFORMATION { public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId; }

		[StructLayout(LayoutKind.Sequential)]
		struct IO_COUNTERS
		{
			public ulong ReadOperationCount; public ulong WriteOperationCount; public ulong OtherOperationCount;
			public ulong ReadTransferCount; public ulong WriteTransferCount; public ulong OtherTransferCount;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct JOBOBJECT_BASIC_LIMIT_INFORMATION
		{
			public long PerProcessUserTimeLimit; public long PerJobUserTimeLimit; public int LimitFlags;
			public UIntPtr MinimumWorkingSetSize; public UIntPtr MaximumWorkingSetSize; public int ActiveProcessLimit;
			public UIntPtr Affinity; public int PriorityClass; public int SchedulingClass;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
		{
			public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; public IO_COUNTERS IoInfo;
			public UIntPtr ProcessMemoryLimit; public UIntPtr JobMemoryLimit;
			public UIntPtr PeakProcessMemoryUsed; public UIntPtr PeakJobMemoryUsed;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
		{
			public long TotalUserTime; public long TotalKernelTime; public long ThisPeriodTotalUserTime;
			public long ThisPeriodTotalKernelTime; public int TotalPageFaultCount; public int TotalProcesses;
			public int ActiveProcesses; public int TotalTerminatedProcesses;
		}

		[StructLayout(LayoutKind.Sequential)]
		struct PROCESS_MEMORY_COUNTERS_EX
		{
			public int cb; public int PageFaultCount; public UIntPtr PeakWorkingSetSize; public UIntPtr WorkingSetSize;
			public UIntPtr QuotaPeakPagedPoolUsage; public UIntPtr QuotaPagedPoolUsage;
			public UIntPtr QuotaPeakNonPagedPoolUsage; public UIntPtr QuotaNonPagedPoolUsage;
			public UIntPtr PagefileUsage; public UIntPtr PeakPagefileUsage; public UIntPtr PrivateUsage;
		}

		[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
		static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int length);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int length, IntPtr returned);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info, int length, IntPtr returned);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int length, IntPtr returned);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool TerminateJobObject(IntPtr job, uint exitCode);

		[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
		static extern IntPtr CreateFileW(string name, uint access, uint share, ref SECURITY_ATTRIBUTES sa, uint disposition, uint flags, IntPtr template);

		[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
		static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
			IntPtr env, string cwd, ref STARTUPINFOEX si, out PROCESS_INFORMATION pi);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr prev, IntPtr retSize);

		[DllImport("kernel32.dll")]
		static extern void DeleteProcThreadAttributeList(IntPtr list);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern uint ResumeThread(IntPtr thread);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern uint WaitForSingleObject(IntPtr handle, uint ms);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool GetExitCodeProcess(IntPtr process, out uint code);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool GetProcessHandleCount(IntPtr process, out int count);

		[DllImport("psapi.dll", SetLastError = true)]
		static extern bool GetProcessMemoryInfo(IntPtr process, out PROCESS_MEMORY_COUNTERS_EX counters, int size);

		[DllImport("kernel32.dll", SetLastError = true)]
		static extern bool CloseHandle(IntPtr handle);

		[DllImport("kernel32.dll")]
		static extern IntPtr GetConsoleWindow();

		static readonly Dictionary<uint, string> CrashNames = new Dictionary<uint, string> {
			{ 0xC0000005, "ACCESS_VIOLATION" }, { 0xC00000FD, "STACK_OVERFLOW" },
			{ 0xC0000409, "STACK_BUFFER_OVERRUN (fail-fast)" }, { 0xC0000374, "HEAP_CORRUPTION" },
			{ 0xC0000094, "INTEGER_DIVIDE_BY_ZERO" }, { 0xC0000095, "INTEGER_OVERFLOW" },
			{ 0xC000001D, "ILLEGAL_INSTRUCTION" }, { 0xC0000017, "NO_MEMORY" },
			{ 0x80000003, "BREAKPOINT" }, { 0xC0000420, "ASSERTION_FAILURE" },
		};

		/// Quotes one argument so CommandLineToArgvW and the MSVC runtime read it back unchanged.
		public static string Quote(string arg)
		{
			if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0)
				return arg;

			var sb = new StringBuilder("\"");
			int backslashes = 0;
			foreach (char c in arg) {
				if (c == '\\') { backslashes++; continue; }
				if (c == '"') sb.Append('\\', backslashes * 2 + 1);
				else sb.Append('\\', backslashes);
				backslashes = 0;
				sb.Append(c);
			}
			sb.Append('\\', backslashes * 2);
			sb.Append('"');
			return sb.ToString();
		}

		public static string JoinArgs(string[] args)
		{
			return string.Join(" ", args.Select(Quote));
		}

		/// The common case in one call: argv, cwd, combined stdout+stderr file, timeout, extra environment.
		public static ProbeResult RunArgs(string[] argv, string workDir, string outputPath, int timeoutMs, IDictionary env)
		{
			return Run(new ProbeOptions {
				CommandLine = JoinArgs(argv), WorkDir = workDir, StdoutPath = outputPath, StderrPath = outputPath,
				TimeoutMs = timeoutMs, Environment = env, MemoryLimitBytes = 2L << 30 });
		}

		public static ProbeResult Run(ProbeOptions o)
		{
			var handles = new List<IntPtr>();
			IntPtr job = IntPtr.Zero;
			IntPtr attrList = IntPtr.Zero;
			IntPtr handleArray = IntPtr.Zero;
			IntPtr envBlock = IntPtr.Zero;
			var pi = new PROCESS_INFORMATION();
			try {
				job = CreateJobObjectW(IntPtr.Zero, null);
				if (job == IntPtr.Zero) throw new Win32Exception();
				var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (o.MemoryLimitBytes > 0) {
					limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
					limits.JobMemoryLimit = (UIntPtr)(ulong)o.MemoryLimitBytes;
				}
				if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref limits, Marshal.SizeOf(limits)))
					throw new Win32Exception();

				IntPtr hIn = OpenStd(o.StdinPath ?? "NUL", false, handles);
				IntPtr hOut = OpenStd(o.StdoutPath ?? "NUL", true, handles);
				IntPtr hErr = (o.StderrPath != null && o.StderrPath == o.StdoutPath)
					? hOut : OpenStd(o.StderrPath ?? "NUL", true, handles);

				var si = new STARTUPINFOEX();
				si.StartupInfo.cb = Marshal.SizeOf(si);
				si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
				si.StartupInfo.hStdInput = hIn;
				si.StartupInfo.hStdOutput = hOut;
				si.StartupInfo.hStdError = hErr;

				// Only the three std handles may be inherited; anything else the
				// host has open (its own output pipes) would keep a reader alive.
				var inheritList = new List<IntPtr> { hIn, hOut };
				if (hErr != hOut) inheritList.Add(hErr);
				handleArray = Marshal.AllocHGlobal(IntPtr.Size * inheritList.Count);
				for (int i = 0; i < inheritList.Count; i++)
					Marshal.WriteIntPtr(handleArray, i * IntPtr.Size, inheritList[i]);
				IntPtr size = IntPtr.Zero;
				InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
				attrList = Marshal.AllocHGlobal(size);
				if (!InitializeProcThreadAttributeList(attrList, 1, 0, ref size)) throw new Win32Exception();
				if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handleArray,
						(IntPtr)(IntPtr.Size * inheritList.Count), IntPtr.Zero, IntPtr.Zero))
					throw new Win32Exception();
				si.lpAttributeList = attrList;

				uint flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
				if (GetConsoleWindow() == IntPtr.Zero) flags |= CREATE_NO_WINDOW;
				envBlock = BuildEnvironment(o.Environment);

				if (!CreateProcessW(null, new StringBuilder(o.CommandLine), IntPtr.Zero, IntPtr.Zero, true, flags,
						envBlock, o.WorkDir, ref si, out pi))
					throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess failed for: " + o.CommandLine);
				if (!AssignProcessToJobObject(job, pi.hProcess)) throw new Win32Exception();

				foreach (var h in handles) CloseHandle(h);
				handles.Clear();

				return Measure(o, job, pi);
			} finally {
				foreach (var h in handles) CloseHandle(h);
				if (pi.hThread != IntPtr.Zero) CloseHandle(pi.hThread);
				if (pi.hProcess != IntPtr.Zero) CloseHandle(pi.hProcess);
				if (job != IntPtr.Zero) CloseHandle(job);
				if (attrList != IntPtr.Zero) { DeleteProcThreadAttributeList(attrList); Marshal.FreeHGlobal(attrList); }
				if (handleArray != IntPtr.Zero) Marshal.FreeHGlobal(handleArray);
				if (envBlock != IntPtr.Zero) Marshal.FreeHGlobal(envBlock);
			}
		}

		static ProbeResult Measure(ProbeOptions o, IntPtr job, PROCESS_INFORMATION pi)
		{
			var r = new ProbeResult();
			var clock = Stopwatch.StartNew();
			ResumeThread(pi.hThread);

			while (true) {
				uint wait = WaitForSingleObject(pi.hProcess, (uint)Math.Max(1, o.SampleMs));
				if (wait != WAIT_TIMEOUT) break;
				Sample(pi.hProcess, clock, r, o.KeepSamples);
				if (clock.ElapsedMilliseconds > o.TimeoutMs) {
					r.TimedOut = true;
					TerminateJobObject(job, 0xDEAD);
					WaitForSingleObject(pi.hProcess, 5000);
					break;
				}
			}
			r.WallMs = clock.Elapsed.TotalMilliseconds;

			uint code;
			GetExitCodeProcess(pi.hProcess, out code);
			r.ExitCode = code;
			string name;
			if (!r.TimedOut && CrashNames.TryGetValue(code, out name)) { r.Crashed = true; r.CrashName = name; }
			else if (!r.TimedOut && code >= 0xC0000000) { r.Crashed = true; r.CrashName = "NTSTATUS 0x" + code.ToString("X8"); }

			PROCESS_MEMORY_COUNTERS_EX mem;
			if (GetProcessMemoryInfo(pi.hProcess, out mem, Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX)))) {
				r.RootPeakWorkingSet = (long)mem.PeakWorkingSetSize;
				r.RootPeakPrivate = (long)mem.PeakPagefileUsage;
			}

			r.LeftoverProcesses = CountLeftovers(job);

			JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
			if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, out acct, Marshal.SizeOf(typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION)), IntPtr.Zero)) {
				r.CpuMs = (acct.TotalUserTime + acct.TotalKernelTime) / 10000.0;
				r.TotalProcesses = acct.TotalProcesses;
			}

			JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
			if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, out ext, Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)), IntPtr.Zero)) {
				r.PeakJobMemory = (long)(ulong)ext.PeakJobMemoryUsed;
				r.PeakProcessMemory = (long)(ulong)ext.PeakProcessMemoryUsed;
			}
			return r;
		}

		// Processes of the job still alive after the root exited. Windows puts
		// the conhost.exe of a console-less console program into the job too;
		// it is not a child of wbsh, so it does not count. Children get one
		// second to finish on their own before they count as left behind.
		static int CountLeftovers(IntPtr job)
		{
			var clock = Stopwatch.StartNew();
			while (true) {
				int alive = 0;
				foreach (int pid in JobProcessIds(job)) {
					try {
						using (var p = Process.GetProcessById(pid))
							if (!string.Equals(p.ProcessName, "conhost", StringComparison.OrdinalIgnoreCase)) alive++;
					} catch (ArgumentException) {
					}
				}
				if (alive == 0 || clock.ElapsedMilliseconds > 1000) return alive;
				System.Threading.Thread.Sleep(50);
			}
		}

		static List<int> JobProcessIds(IntPtr job)
		{
			var ids = new List<int>();
			int capacity = 64;
			int size = 8 + IntPtr.Size * capacity;
			IntPtr buffer = Marshal.AllocHGlobal(size);
			try {
				if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, buffer, size, IntPtr.Zero)) return ids;
				int count = Marshal.ReadInt32(buffer, 4);
				for (int i = 0; i < count && i < capacity; i++)
					ids.Add((int)Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size));
			} finally {
				Marshal.FreeHGlobal(buffer);
			}
			return ids;
		}

		static void Sample(IntPtr process, Stopwatch clock, ProbeResult r, bool keep)
		{
			int count;
			if (GetProcessHandleCount(process, out count) && count > r.PeakHandles) r.PeakHandles = count;
			if (!keep) return;

			PROCESS_MEMORY_COUNTERS_EX mem;
			if (!GetProcessMemoryInfo(process, out mem, Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX)))) return;
			r.Samples.Add(new ProbeSample {
				Ms = clock.Elapsed.TotalMilliseconds, PrivateBytes = (long)mem.PrivateUsage,
				WorkingSet = (long)mem.WorkingSetSize, Handles = count });
		}

		static IntPtr OpenStd(string path, bool write, List<IntPtr> handles)
		{
			var sa = new SECURITY_ATTRIBUTES { nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)), bInheritHandle = true };
			IntPtr h = CreateFileW(path, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, ref sa,
				write ? CREATE_ALWAYS : OPEN_EXISTING, 0, IntPtr.Zero);
			if (h == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error(), "cannot open " + path);
			handles.Add(h);
			return h;
		}

		static IntPtr BuildEnvironment(IDictionary overrides)
		{
			var env = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			foreach (DictionaryEntry e in System.Environment.GetEnvironmentVariables())
				env[(string)e.Key] = (string)e.Value;
			if (overrides != null) {
				foreach (DictionaryEntry e in overrides) {
					if (e.Value == null) env.Remove((string)e.Key);
					else env[(string)e.Key] = e.Value.ToString();
				}
			}
			var sb = new StringBuilder();
			foreach (var kv in env) sb.Append(kv.Key).Append('=').Append(kv.Value).Append('\0');
			sb.Append('\0');
			return Marshal.StringToHGlobalUni(sb.ToString());
		}
	}
}
