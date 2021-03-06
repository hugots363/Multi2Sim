/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>
#include <math.h>

#include <arch/common/arch.h>
#include <arch/x86/emu/checkpoint.h>
#include <arch/x86/emu/context.h>
#include <arch/x86/emu/emu.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/config.h>
#include <lib/util/debug.h>
#include <lib/util/file.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>
#include <lib/util/misc.h>
#include <lib/util/stats.h>
#include <lib/util/string.h>
#include <lib/util/timer.h>
#include <mem-system/memory.h>
#include <mem-system/module.h>
#include <mem-system/prefetch-history.h>

#include "bpred.h"
#include "cpu.h"
#include "event-queue.h"
#include "fetch-queue.h"
#include "fu.h"
#include "inst-queue.h"
#include "load-store-queue.h"
#include "mem-config.h"
#include "reg-file.h"
#include "rob.h"
#include "trace-cache.h"
#include "uop-queue.h"


/*
 * Global variables
 */


/* Help message */

char *x86_config_help =
	"The x86 CPU configuration file is a plain text INI file, defining\n"
	"the parameters of the CPU model used for a detailed (architectural) simulation.\n"
	"This configuration file is passed to Multi2Sim with option '--x86-config <file>,\n"
	"which must be accompanied by option '--x86-sim detailed'.\n"
	"\n"
	"The following is a list of the sections allowed in the CPU configuration file,\n"
	"along with the list of variables for each section.\n"
	"\n"
	"Section '[ General ]':\n"
	"\n"
	"  Frequency = <freq> (Default = 3000 MHz)\n"
	"      Frequency in MHz for the x86 CPU. Value between 1 and 10K.\n"
	"  Cores = <num_cores> (Default = 1)\n"
	"      Number of cores.\n"
	"  Threads = <num_threads> (Default = 1)\n"
	"      Number of hardware threads per core. The total number of computing nodes\n"
	"      in the CPU model is equals to Cores * Threads.\n"
	"  FastForward = <num_inst> (Default = 0)\n"
	"      Number of x86 instructions to run with a fast functional simulation before\n"
	"      the architectural simulation starts.\n"
	"  ContextQuantum = <cycles> (Default = 100k)\n"
	"      If ContextSwitch is true, maximum number of cycles that a context can occupy\n"
	"      a CPU hardware thread before it is replaced by other pending context.\n"
	"  ThreadQuantum = <cycles> (Default = 1k)\n"
	"      For multithreaded processors (Threads > 1) configured as coarse-grain multi-\n"
	"      threading (FetchKind = SwitchOnEvent), number of cycles in which instructions\n"
	"      are fetched from the same thread before switching.\n"
	"  ThreadSwitchPenalty = <cycles> (Default = 0)\n"
	"      For coarse-grain multithreaded processors (FetchKind = SwitchOnEvent), number\n"
	"      of cycles that the fetch stage stalls after a thread switch.\n"
	"  RecoverKind = {Writeback|Commit} (Default = Writeback)\n"
	"      On branch misprediction, stage in the execution of the mispredicted branch\n"
	"      when processor recovery is triggered.\n"
	"  RecoverPenalty = <cycles> (Default = 0)\n"
	"      Number of cycles that the fetch stage gets stalled after a branch\n"
	"      misprediction.\n"
	"  PageSize = <size> (Default = 4kB)\n"
	"      Memory page size in bytes.\n"
	"  DataCachePerfect = {t|f} (Default = False)\n"
	"  ProcessPrefetchHints = {t|f} (Default = True)\n"
	"      If specified as false, the cpu will ignore any prefetch hints/instructions.\n"
	"  PrefetchHistorySize = <size> (Default = 10)\n"
	"      Number of past prefetches to keep track of, so as to avoid redundant prefetches\n"
	"      from being issued from the cpu to the cache module.\n"
	"  InstructionCachePerfect = {t|f} (Default = False)\n"
	"      Set these options to true to simulate a perfect data/instruction caches,\n"
	"      respectively, where every access results in a hit. If set to false, the\n"
	"      parameters of the caches are given in the memory configuration file\n"
	"\n"
	"Section '[ Pipeline ]':\n"
	"\n"
	"  FetchKind = {Shared|TimeSlice|SwitchOnEvent} (Default = TimeSlice)\n"
	"      Policy for fetching instruction from different threads. A shared fetch stage\n"
	"      fetches instructions from different threads in the same cycle; a time-slice\n"
	"      fetch switches between threads in a round-robin fashion; option SwitchOnEvent\n"
	"      switches thread fetch on long-latency operations or thread quantum expiration.\n"
	"  DecodeWidth = <num_inst> (Default = 4)\n"
	"      Number of x86 instructions decoded per cycle.\n"
	"  DispatchKind = {Shared|TimeSlice} (Default = TimeSlice)\n"
	"      Policy for dispatching instructions from different threads. If shared,\n"
	"      instructions from different threads are dispatched in the same cycle. Otherwise,\n"
	"      instruction dispatching is done in a round-robin fashion at a cycle granularity.\n"
	"  DispatchWidth = <num_inst> (Default = 4)\n"
	"      Number of microinstructions dispatched per cycle.\n"
	"  IssueKind = {Shared|TimeSlice} (Default = TimeSlice)\n"
	"      Policy for issuing instructions from different threads. If shared, instructions\n"
	"      from different threads are issued in the same cycle; otherwise, instruction issue\n"
	"      is done round-robin at a cycle granularity.\n"
	"  IssueWidth = <num_inst> (Default = 4)\n"
	"      Number of microinstructions issued per cycle.\n"
	"  CommitKind = {Shared|TimeSlice} (Default = Shared)\n"
	"      Policy for committing instructions from different threads. If shared,\n"
	"      instructions from different threads are committed in the same cycle; otherwise,\n"
	"      they commit in a round-robin fashion.\n"
	"  CommitWidth = <num_inst> (Default = 4)\n"
	"      Number of microinstructions committed per cycle.\n"
	"  OccupancyStats = {t|f} (Default = False)\n"
	"      Calculate structures occupancy statistics. Since this computation requires\n"
	"      additional overhead, the option needs to be enabled explicitly. These statistics\n"
	"      will be attached to the CPU report.\n"
	"\n"
	"Section '[ Queues ]':\n"
	"\n"
	"  FetchQueueSize = <bytes> (Default = 64)\n"
	"      Size of the fetch queue given in bytes.\n"
	"  UopQueueSize = <num_uops> (Default = 32)\n"
	"      Size of the uop queue size, given in number of uops.\n"
	"  RobKind = {Private|Shared} (Default = Private)\n"
	"      Reorder buffer sharing among hardware threads.\n"
	"  RobSize = <num_uops> (Default = 64)\n"
	"      Reorder buffer size in number of microinstructions (if private, per-thread size).\n"
	"  IqKind = {Private|Shared} (Default = Private)\n"
	"      Instruction queue sharing among threads.\n"
	"  IqSize = <num_uops> (Default = 40)\n"
	"      Instruction queue size in number of uops (if private, per-thread IQ size).\n"
	"  LsqKind = {Private|Shared} (Default = 20)\n"
	"      Load-store queue sharing among threads.\n"
	"  LsqSize = <num_uops> (Default = 20)\n"
	"      Load-store queue size in number of uops (if private, per-thread LSQ size).\n"
	"  RfKind = {Private|Shared} (Default = Private)\n"
	"      Register file sharing among threads.\n"
	"  RfIntSize = <entries> (Default = 80)\n"
	"      Number of integer physical register (if private, per-thread).\n"
	"  RfFpSize = <entries> (Default = 40)\n"
	"      Number of floating-point physical registers (if private, per-thread).\n"
	"  RfXmmSize = <entries> (Default = 40)\n"
	"      Number of XMM physical registers (if private, per-thread).\n"
	"\n"
	"Section '[ TraceCache ]':\n"
	"\n"
	"  Present = {t|f} (Default = False)\n"
	"      If true, a trace cache is included in the model. If false, the rest of the\n"
	"      options in this section are ignored.\n"
	"  Sets = <num_sets> (Default = 64)\n"
	"      Number of sets in the trace cache.\n"
	"  Assoc = <num_ways> (Default = 4)\n"
	"      Associativity of the trace cache. The product Sets * Assoc is the total\n"
	"      number of traces that can be stored in the trace cache.\n"
	"  TraceSize = <num_uops> (Default = 16)\n"
	"      Maximum size of a trace of uops.\n"
	"  BranchMax = <num_branches> (Default = 3)\n"
	"      Maximum number of branches contained in a trace.\n"
	"  QueueSize = <num_uops> (Default = 32)\n"
	"      Size of the trace queue size in uops.\n"
	"\n"
	"Section '[ FunctionalUnits ]':\n"
	"\n"
	"  The possible variables in this section follow the format\n"
	"      <func_unit>.<field> = <value>\n"
	"  where <func_unit> refers to a functional unit type, and <field> refers to a\n"
	"  property of it. Possible values for <func_unit> are:\n"
	"\n"
	"      IntAdd      Integer adder\n"
	"      IntMult     Integer multiplier\n"
	"      IntDiv      Integer divider\n"
	"\n"
	"      EffAddr     Operator for effective address computations\n"
	"      Logic       Operator for logic operations\n"
	"\n"
	"      FloatSimple    Simple floating-point operations\n"
	"      FloatAdd       Floating-point adder\n"
	"      FloatComp      Floating-point comparator\n"
	"      FloatMult      Floating-point multiplier\n"
	"      FloatDiv       Floating-point divider\n"
	"      FloatComplex   Operator for complex floating-point computations\n"
	"\n"
	"      XMMIntAdd      XMM integer adder\n"
	"      XMMIntMult     XMM integer multiplier\n"
	"      XMMIntDiv      XMM integer Divider\n"
	"\n"
	"      XMMLogic       XMM logic operations\n"
	"\n"
	"      XMMFloatAdd       XMM floating-point adder\n"
	"      XMMFloatComp      XMM floating-point comparator\n"
	"      XMMFloatMult      XMM floating-point multiplier\n"
	"      XMMFloatDiv       XMM floating-point divider\n"
	"      XMMFloatConv      XMM floating-point converter\n"
	"      XMMFloatComplex   Complex XMM floating-point operations\n"
	"\n"
	"  Possible values for <field> are:\n"
	"      Count       Number of functional units of a given kind.\n"
	"      OpLat       Latency of the operator.\n"
	"      IssueLat    Latency since an instruction was issued until the functional\n"
	"                  unit is available for the next use. For pipelined operators,\n"
	"                  IssueLat is smaller than OpLat.\n"
	"\n"
	"Section '[ BranchPredictor ]':\n"
	"\n"
	"  Kind = {Perfect|Taken|NotTaken|Bimodal|TwoLevel|Combined} (Default = TwoLevel)\n"
	"      Branch predictor type.\n"
	"  BTB.Sets = <num_sets> (Default = 256)\n"
	"      Number of sets in the BTB.\n"
	"  BTB.Assoc = <num_ways) (Default = 4)\n"
	"      BTB associativity.\n"
	"  Bimod.Size = <entries> (Default = 1024)\n"
	"      Number of entries of the bimodal branch predictor.\n"
	"  Choice.Size = <entries> (Default = 1024)\n"
	"      Number of entries for the choice predictor.\n"
	"  RAS.Size = <entries> (Default = 32)\n"
	"      Number of entries of the return address stack (RAS).\n"
	"  TwoLevel.L1Size = <entries> (Default = 1)\n"
	"      For the two-level adaptive predictor, level 1 size.\n"
	"  TwoLevel.L2Size = <entries> (Default = 1024)\n"
	"      For the two-level adaptive predictor, level 2 size.\n"
	"  TwoLevel.HistorySize = <size> (Default = 8)\n"
	"      For the two-level adaptive predictor, level 2 history size.\n"
	"\n";


/* Main Processor Global Variable */
struct x86_cpu_t *x86_cpu;

/* Trace */
int x86_trace_category;


/* Configuration file and parameters */

char *x86_config_file_name = "";
char *x86_cpu_report_file_name = "";

int x86_cpu_num_cores = 1;
int x86_cpu_num_threads = 1;

long long x86_cpu_fast_forward_count;
long long x86_cpu_warm_up_count;
char *x86_save_checkpoint_after_warm_up_file_name;

int x86_cpu_context_quantum;
int x86_cpu_thread_quantum;
int x86_cpu_thread_switch_penalty;

char *x86_cpu_recover_kind_map[] = { "Writeback", "Commit" };
enum x86_cpu_recover_kind_t x86_cpu_recover_kind;
int x86_cpu_recover_penalty;

char *x86_cpu_fetch_kind_map[] = { "Shared", "TimeSlice", "SwitchOnEvent" };
enum x86_cpu_fetch_kind_t x86_cpu_fetch_kind;

int x86_cpu_decode_width;

char *x86_cpu_dispatch_kind_map[] = { "Shared", "TimeSlice" };
enum x86_cpu_dispatch_kind_t x86_cpu_dispatch_kind;
int x86_cpu_dispatch_width;

char *x86_cpu_issue_kind_map[] = { "Shared", "TimeSlice" };
enum x86_cpu_issue_kind_t x86_cpu_issue_kind;
int x86_cpu_issue_width;

char *x86_cpu_commit_kind_map[] = { "Shared", "TimeSlice" };
enum x86_cpu_commit_kind_t x86_cpu_commit_kind;
int x86_cpu_commit_width;

int x86_cpu_occupancy_stats;

struct str_map_t x86_dispatch_stall_map =
{
	x86_dispatch_stall_max, {
		{ "disp-used", x86_dispatch_stall_used },
		{ "disp-used-spec", x86_dispatch_stall_spec },
		{ "disp-stall-uopq", x86_dispatch_stall_uop_queue },
		{ "disp-stall-rob-smt", x86_dispatch_stall_rob_smt },
		{ "disp-stall-rob-mem", x86_dispatch_stall_rob_mem },
		{ "disp-stall-rob", x86_dispatch_stall_rob },
		{ "disp-stall-iq", x86_dispatch_stall_iq },
		{ "disp-stall-lq", x86_dispatch_stall_lq },
		{ "disp-stall-sq", x86_dispatch_stall_sq },
		{ "disp-stall-pq", x86_dispatch_stall_pq },
		{ "disp-stall-rename", x86_dispatch_stall_rename },
		{ "disp-stall-ctx", x86_dispatch_stall_ctx }
	}
};


/*
 * Private Functions
 */


static char *x86_cpu_err_fast_forward =
	"\tThe number of instructions specified in the x86 CPU configuration file\n"
	"\tfor fast-forward (functional) execution has caused all contexts to end\n"
	"\tbefore the timing simulation could start. Please decrease the number\n"
	"\tof fast-forward instructions and retry.\n";


static void x86_cpu_thread_mapping_report_init(int core, int thread);


/* Dump the CPU configuration */
static void x86_cpu_config_dump(FILE *f)
{
	/* General configuration */
	fprintf(f, "[ Config.General ]\n");
	fprintf(f, "Frequency = %d\n", arch_x86->frequency);
	fprintf(f, "Cores = %d\n", x86_cpu_num_cores);
	fprintf(f, "Threads = %d\n", x86_cpu_num_threads);
	fprintf(f, "FastForward = %lld\n", x86_cpu_fast_forward_count);
	fprintf(f, "ContextQuantum = %d\n", x86_cpu_context_quantum);
	fprintf(f, "ThreadQuantum = %d\n", x86_cpu_thread_quantum);
	fprintf(f, "ThreadSwitchPenalty = %d\n", x86_cpu_thread_switch_penalty);
	fprintf(f, "RecoverKind = %s\n", x86_cpu_recover_kind_map[x86_cpu_recover_kind]);
	fprintf(f, "RecoverPenalty = %d\n", x86_cpu_recover_penalty);
	fprintf(f, "ProcessPrefetchHints = %d\n", x86_emu_process_prefetch_hints);
	fprintf(f, "PrefetchHistorySize = %d\n", prefetch_history_size);
	fprintf(f, "\n");

	/* Pipeline */
	fprintf(f, "[ Config.Pipeline ]\n");
	fprintf(f, "FetchKind = %s\n", x86_cpu_fetch_kind_map[x86_cpu_fetch_kind]);
	fprintf(f, "DecodeWidth = %d\n", x86_cpu_decode_width);
	fprintf(f, "DispatchKind = %s\n", x86_cpu_dispatch_kind_map[x86_cpu_dispatch_kind]);
	fprintf(f, "DispatchWidth = %d\n", x86_cpu_dispatch_width);
	fprintf(f, "IssueKind = %s\n", x86_cpu_issue_kind_map[x86_cpu_issue_kind]);
	fprintf(f, "IssueWidth = %d\n", x86_cpu_issue_width);
	fprintf(f, "CommitKind = %s\n", x86_cpu_commit_kind_map[x86_cpu_commit_kind]);
	fprintf(f, "CommitWidth = %d\n", x86_cpu_commit_width);
	fprintf(f, "OccupancyStats = %s\n", x86_cpu_occupancy_stats ? "True" : "False");
	fprintf(f, "\n");

	/* Queues */
	fprintf(f, "[ Config.Queues ]\n");
	fprintf(f, "FetchQueueSize = %d\n", x86_fetch_queue_size);
	fprintf(f, "UopQueueSize = %d\n", x86_uop_queue_size);
	fprintf(f, "RobKind = %s\n", x86_rob_kind_map[x86_rob_kind]);
	fprintf(f, "RobSize = %d\n", x86_rob_size);
	fprintf(f, "IqKind = %s\n", x86_iq_kind_map[x86_iq_kind]);
	fprintf(f, "IqSize = %d\n", x86_iq_size);
	fprintf(f, "LsqKind = %s\n", x86_lsq_kind_map[x86_lsq_kind]);
	fprintf(f, "LqSize = %d\n", x86_lq_size);
	fprintf(f, "SqSize = %d\n", x86_sq_size);
	fprintf(f, "PqSize = %d\n", x86_pq_size);
	fprintf(f, "RfKind = %s\n", x86_reg_file_kind_map[x86_reg_file_kind]);
	fprintf(f, "RfIntSize = %d\n", x86_reg_file_int_size);
	fprintf(f, "RfFpSize = %d\n", x86_reg_file_fp_size);
	fprintf(f, "\n");

	/* Trace Cache */
	fprintf(f, "[ Config.TraceCache ]\n");
	fprintf(f, "Present = %s\n", x86_trace_cache_present ? "True" : "False");
	fprintf(f, "Sets = %d\n", x86_trace_cache_num_sets);
	fprintf(f, "Assoc = %d\n", x86_trace_cache_assoc);
	fprintf(f, "TraceSize = %d\n", x86_trace_cache_trace_size);
	fprintf(f, "BranchMax = %d\n", x86_trace_cache_branch_max);
	fprintf(f, "QueueSize = %d\n", x86_trace_cache_queue_size);
	fprintf(f, "\n");

	/* Functional units */
	x86_fu_config_dump(f);

	/* Branch Predictor */
	fprintf(f, "[ Config.BranchPredictor ]\n");
	fprintf(f, "Kind = %s\n", x86_bpred_kind_map[x86_bpred_kind]);
	fprintf(f, "BTB.Sets = %d\n", x86_bpred_btb_sets);
	fprintf(f, "BTB.Assoc = %d\n", x86_bpred_btb_assoc);
	fprintf(f, "Bimod.Size = %d\n", x86_bpred_bimod_size);
	fprintf(f, "Choice.Size = %d\n", x86_bpred_choice_size);
	fprintf(f, "RAS.Size = %d\n", x86_bpred_ras_size);
	fprintf(f, "TwoLevel.L1Size = %d\n", x86_bpred_twolevel_l1size);
	fprintf(f, "TwoLevel.L2Size = %d\n", x86_bpred_twolevel_l2size);
	fprintf(f, "TwoLevel.HistorySize = %d\n", x86_bpred_twolevel_hist_size);
	fprintf(f, "\n");

	/* End of configuration */
	fprintf(f, "\n");

}


static void x86_cpu_dump_uop_report(FILE *f, long long *uop_stats, char *prefix, int peak_ipc)
{
	long long uinst_int_count = 0;
	long long uinst_logic_count = 0;
	long long uinst_fp_count = 0;
	long long uinst_mem_count = 0;
	long long uinst_ctrl_count = 0;
	long long uinst_total = 0;

	char *name;
	enum x86_uinst_flag_t flags;
	int i;

	for (i = 0; i < x86_uinst_opcode_count; i++)
	{
		name = x86_uinst_info[i].name;
		flags = x86_uinst_info[i].flags;

		fprintf(f, "%s.Uop.%s = %lld\n", prefix, name, uop_stats[i]);
		if (flags & X86_UINST_INT)
			uinst_int_count += uop_stats[i];
		if (flags & X86_UINST_LOGIC)
			uinst_logic_count += uop_stats[i];
		if (flags & X86_UINST_FP)
			uinst_fp_count += uop_stats[i];
		if (flags & X86_UINST_MEM)
			uinst_mem_count += uop_stats[i];
		if (flags & X86_UINST_CTRL)
			uinst_ctrl_count += uop_stats[i];
		uinst_total += uop_stats[i];
	}
	fprintf(f, "%s.Integer = %lld\n", prefix, uinst_int_count);
	fprintf(f, "%s.Logic = %lld\n", prefix, uinst_logic_count);
	fprintf(f, "%s.FloatingPoint = %lld\n", prefix, uinst_fp_count);
	fprintf(f, "%s.Memory = %lld\n", prefix, uinst_mem_count);
	fprintf(f, "%s.Ctrl = %lld\n", prefix, uinst_ctrl_count);
	fprintf(f, "%s.WndSwitch = %lld\n", prefix, uop_stats[x86_uinst_call] + uop_stats[x86_uinst_ret]);
	fprintf(f, "%s.Total = %lld\n", prefix, uinst_total);
	fprintf(f, "%s.IPC = %.4g\n", prefix, arch_x86->cycle ? (double) uinst_total / arch_x86->cycle : 0.0);
	fprintf(f, "%s.DutyCycle = %.4g\n", prefix, arch_x86->cycle && peak_ipc ?
		(double) uinst_total / arch_x86->cycle / peak_ipc : 0.0);
	fprintf(f, "\n");
}


#define DUMP_DISPATCH_STAT(NAME) { \
	fprintf(f, "Dispatch.Stall." #NAME " = %lld\n", X86_CORE.dispatch_stall[x86_dispatch_stall_##NAME]); \
}

#define DUMP_CORE_STRUCT_STATS(NAME, ITEM) { \
	fprintf(f, #NAME ".Size = %d\n", (int) x86_##ITEM##_size * x86_cpu_num_threads); \
	if (x86_cpu_occupancy_stats) \
		fprintf(f, #NAME ".Occupancy = %.2f\n", arch_x86->cycle ? (double) X86_CORE.ITEM##_occupancy / arch_x86->cycle : 0.0); \
	fprintf(f, #NAME ".Full = %lld\n", X86_CORE.ITEM##_full); \
	fprintf(f, #NAME ".Reads = %lld\n", X86_CORE.ITEM##_reads); \
	fprintf(f, #NAME ".Writes = %lld\n", X86_CORE.ITEM##_writes); \
}

#define DUMP_THREAD_STRUCT_STATS(NAME, ITEM) { \
	fprintf(f, #NAME ".Size = %d\n", (int) x86_##ITEM##_size); \
	if (x86_cpu_occupancy_stats) \
		fprintf(f, #NAME ".Occupancy = %.2f\n", arch_x86->cycle ? (double) X86_THREAD.ITEM##_occupancy / arch_x86->cycle : 0.0); \
	fprintf(f, #NAME ".Full = %lld\n", X86_THREAD.ITEM##_full); \
	fprintf(f, #NAME ".Reads = %lld\n", X86_THREAD.ITEM##_reads); \
	fprintf(f, #NAME ".Writes = %lld\n", X86_THREAD.ITEM##_writes); \
}


static void x86_cpu_dump_report(void)
{
	FILE *f;
	int core, thread;

	long long now;

	/* Open file */
	f = file_open_for_write(x86_cpu_report_file_name);
	if (!f)
		return;

	/* Get CPU timer value */
	now = m2s_timer_get_value(arch_x86->timer);

	/* Dump CPU configuration */
	fprintf(f, ";\n; CPU Configuration\n;\n\n");
	x86_cpu_config_dump(f);

	/* Report for the complete processor */
	fprintf(f, ";\n; Simulation Statistics\n;\n\n");
	fprintf(f, "; Global statistics\n");
	fprintf(f, "[ Global ]\n\n");
	fprintf(f, "Cycles = %lld\n", arch_x86->cycle);
	fprintf(f, "Time = %.2f\n", (double) now / 1000000);
	fprintf(f, "CyclesPerSecond = %.0f\n", now ? (double) arch_x86->cycle / now * 1000000 : 0.0);
	fprintf(f, "MemoryUsed = %lu\n", (long) mem_mapped_space);
	fprintf(f, "MemoryUsedMax = %lu\n", (long) mem_max_mapped_space);
	fprintf(f, "\n");

	/* Dispatch stage */
	fprintf(f, "; Dispatch stage\n");
	x86_cpu_dump_uop_report(f, x86_cpu->num_dispatched_uinst_array, "Dispatch", x86_cpu_dispatch_width);

	/* Issue stage */
	fprintf(f, "; Issue stage\n");
	x86_cpu_dump_uop_report(f, x86_cpu->num_issued_uinst_array, "Issue", x86_cpu_issue_width);

	/* Commit stage */
	fprintf(f, "; Commit stage\n");
	x86_cpu_dump_uop_report(f, x86_cpu->num_committed_uinst_array, "Commit", x86_cpu_commit_width);

	/* Committed branches */
	fprintf(f, "; Committed branches\n");
	fprintf(f, ";    Branches - Number of committed control uops\n");
	fprintf(f, ";    Squashed - Number of mispredicted uops squashed from the ROB\n");
	fprintf(f, ";    Mispred - Number of mispredicted branches in the correct path\n");
	fprintf(f, ";    PredAcc - Prediction accuracy\n");
	fprintf(f, "Commit.Branches = %lld\n", x86_cpu->num_branch_uinst);
	fprintf(f, "Commit.Squashed = %lld\n", x86_cpu->num_squashed_uinst);
	fprintf(f, "Commit.Mispred = %lld\n", x86_cpu->num_mispred_branch_uinst);
	fprintf(f, "Commit.PredAcc = %.4g\n", x86_cpu->num_branch_uinst ?
		(double) (x86_cpu->num_branch_uinst - x86_cpu->num_mispred_branch_uinst) / x86_cpu->num_branch_uinst : 0.0);
	fprintf(f, "\n");

	/* Report for each core */
	X86_CORE_FOR_EACH
	{
		/* Core */
		fprintf(f, "\n; Statistics for core %d\n", core);
		fprintf(f, "[ c%d ]\n\n", core);

		/* Functional units */
		x86_fu_dump_report(X86_CORE.fu, f);

		/* Dispatch slots */
		if (x86_cpu_dispatch_kind == x86_cpu_dispatch_kind_timeslice)
		{
			fprintf(f, "; Dispatch slots usage (sum = cycles * dispatch width)\n");
			fprintf(f, ";    used - dispatch slot was used by a non-spec uop\n");
			fprintf(f, ";    spec - used by a mispeculated uop\n");
			fprintf(f, ";    ctx - no context allocated to thread\n");
			fprintf(f, ";    uopq,rob,iq,lsq,rename - no space in structure\n");
			DUMP_DISPATCH_STAT(used);
			DUMP_DISPATCH_STAT(spec);
			DUMP_DISPATCH_STAT(uop_queue);
			DUMP_DISPATCH_STAT(rob);
			DUMP_DISPATCH_STAT(iq);
			DUMP_DISPATCH_STAT(lq);
			DUMP_DISPATCH_STAT(sq);
			DUMP_DISPATCH_STAT(pq);
			DUMP_DISPATCH_STAT(rename);
			DUMP_DISPATCH_STAT(ctx);
			fprintf(f, "\n");
		}

		/* Dispatch stage */
		fprintf(f, "; Dispatch stage\n");
		x86_cpu_dump_uop_report(f, X86_CORE.num_dispatched_uinst_array, "Dispatch", x86_cpu_dispatch_width);

		/* Issue stage */
		fprintf(f, "; Issue stage\n");
		x86_cpu_dump_uop_report(f, X86_CORE.num_issued_uinst_array, "Issue", x86_cpu_issue_width);

		/* Commit stage */
		fprintf(f, "; Commit stage\n");
		x86_cpu_dump_uop_report(f, X86_CORE.num_committed_uinst_array, "Commit", x86_cpu_commit_width);

		/* Committed branches */
		fprintf(f, "; Committed branches\n");
		fprintf(f, "Commit.Branches = %lld\n", X86_CORE.num_branch_uinst);
		fprintf(f, "Commit.Squashed = %lld\n", X86_CORE.num_squashed_uinst);
		fprintf(f, "Commit.Mispred = %lld\n", X86_CORE.num_mispred_branch_uinst);
		fprintf(f, "Commit.PredAcc = %.4g\n", X86_CORE.num_branch_uinst ?
			(double) (X86_CORE.num_branch_uinst - X86_CORE.num_mispred_branch_uinst) / X86_CORE.num_branch_uinst : 0.0);
		fprintf(f, "\n");

		/* Occupancy stats */
		fprintf(f, "; Structure statistics (reorder buffer, instruction queue,\n");
		fprintf(f, "; load-store queue, and integer/floating-point register file)\n");
		fprintf(f, ";    Size - Available size\n");
		fprintf(f, ";    Occupancy - Average number of occupied entries\n");
		fprintf(f, ";    Full - Number of cycles when the structure was full\n");
		fprintf(f, ";    Reads, Writes - Accesses to the structure\n");
		if (x86_rob_kind == x86_rob_kind_shared)
			DUMP_CORE_STRUCT_STATS(ROB, rob);
		if (x86_iq_kind == x86_iq_kind_shared)
		{
			DUMP_CORE_STRUCT_STATS(IQ, iq);
			fprintf(f, "IQ.WakeupAccesses = %lld\n", X86_CORE.iq_wakeup_accesses);
		}
		if (x86_lsq_kind == x86_lsq_kind_shared)
		{
			DUMP_CORE_STRUCT_STATS(LSQ, lq);
			DUMP_CORE_STRUCT_STATS(LSQ, sq);
			DUMP_CORE_STRUCT_STATS(LSQ, pq);
		}
		if (x86_reg_file_kind == x86_reg_file_kind_shared)
		{
			DUMP_CORE_STRUCT_STATS(RF_Int, reg_file_int);
			DUMP_CORE_STRUCT_STATS(RF_Fp, reg_file_fp);
		}
		fprintf(f, "\n");

		/* Report for each thread */
		X86_THREAD_FOR_EACH
		{
			fprintf(f, "\n; Statistics for core %d - thread %d\n", core, thread);
			fprintf(f, "[ c%dt%d ]\n\n", core, thread);

			/* Dispatch stage */
			fprintf(f, "; Dispatch stage\n");
			x86_cpu_dump_uop_report(f, X86_THREAD.num_dispatched_uinst_array, "Dispatch", x86_cpu_dispatch_width);

			/* Issue stage */
			fprintf(f, "; Issue stage\n");
			x86_cpu_dump_uop_report(f, X86_THREAD.num_issued_uinst_array, "Issue", x86_cpu_issue_width);

			/* Commit stage */
			fprintf(f, "; Commit stage\n");
			x86_cpu_dump_uop_report(f, X86_THREAD.num_committed_uinst_array, "Commit", x86_cpu_commit_width);

			/* Committed branches */
			fprintf(f, "; Committed branches\n");
			fprintf(f, "Commit.Branches = %lld\n", X86_THREAD.num_branch_uinst);
			fprintf(f, "Commit.Squashed = %lld\n", X86_THREAD.num_squashed_uinst);
			fprintf(f, "Commit.Mispred = %lld\n", X86_THREAD.num_mispred_branch_uinst);
			fprintf(f, "Commit.PredAcc = %.4g\n", X86_THREAD.num_branch_uinst ?
				(double) (X86_THREAD.num_branch_uinst - X86_THREAD.num_mispred_branch_uinst) / X86_THREAD.num_branch_uinst : 0.0);
			fprintf(f, "\n");

			/* Occupancy stats */
			fprintf(f, "; Structure statistics (reorder buffer, instruction queue, load-store queue,\n");
			fprintf(f, "; integer/floating-point register file, and renaming table)\n");
			if (x86_rob_kind == x86_rob_kind_private)
				DUMP_THREAD_STRUCT_STATS(ROB, rob);
			if (x86_iq_kind == x86_iq_kind_private)
			{
				DUMP_THREAD_STRUCT_STATS(IQ, iq);
				fprintf(f, "IQ.WakeupAccesses = %lld\n", X86_THREAD.iq_wakeup_accesses);
			}
			if (x86_lsq_kind == x86_lsq_kind_private)
			{
				DUMP_THREAD_STRUCT_STATS(LSQ, lq);
				DUMP_THREAD_STRUCT_STATS(LSQ, sq);
				DUMP_THREAD_STRUCT_STATS(LSQ, pq);
			}
			if (x86_reg_file_kind == x86_reg_file_kind_private)
			{
				DUMP_THREAD_STRUCT_STATS(RF_Int, reg_file_int);
				DUMP_THREAD_STRUCT_STATS(RF_Fp, reg_file_fp);
			}
			fprintf(f, "RAT.IntReads = %lld\n", X86_THREAD.rat_int_reads);
			fprintf(f, "RAT.IntWrites = %lld\n", X86_THREAD.rat_int_writes);
			fprintf(f, "RAT.FpReads = %lld\n", X86_THREAD.rat_fp_reads);
			fprintf(f, "RAT.FpWrites = %lld\n", X86_THREAD.rat_fp_writes);
			fprintf(f, "BTB.Reads = %lld\n", X86_THREAD.btb_reads);
			fprintf(f, "BTB.Writes = %lld\n", X86_THREAD.btb_writes);
			fprintf(f, "\n");

			/* Trace cache stats */
			if (X86_THREAD.trace_cache)
				x86_trace_cache_dump_report(X86_THREAD.trace_cache, f);
		}
	}

	/* Close */
	fclose(f);
}


static void x86_cpu_thread_init(int core, int thread)
{
	x86_cpu_thread_mapping_report_init(core, thread);
}


static void x86_cpu_core_init(int core)
{
	int thread;
	X86_CORE.thread = xcalloc(x86_cpu_num_threads, sizeof(struct x86_thread_t));
	X86_THREAD_FOR_EACH
		x86_cpu_thread_init(core, thread);

	X86_CORE.prefetch_history = prefetch_history_create();
}


static void x86_cpu_thread_done(int core, int thread)
{
	if (X86_THREAD.report_stack)
	{
		free(X86_THREAD.report_stack->hits_per_level_int);
		free(X86_THREAD.report_stack->stream_hits_per_level_int);
		free(X86_THREAD.report_stack->misses_per_level_int);
		free(X86_THREAD.report_stack->retries_per_level_int);
		free(X86_THREAD.report_stack->evictions_per_level_int);
		file_close(X86_THREAD.report_stack->report_file);
	}
	free(X86_THREAD.report_stack);
	for (int level = 1; level <= max_mod_level; level++)
		list_free(X86_THREAD.reachable_modules_per_level[level]);
	free(X86_THREAD.reachable_modules_per_level);
	file_close(X86_THREAD.mapping_report_file);
}


static void x86_cpu_core_done(int core)
{
	int thread;
	X86_THREAD_FOR_EACH
		x86_cpu_thread_done(core, thread);
	free(X86_CORE.thread);
	prefetch_history_free(X86_CORE.prefetch_history);
}


/*
 * Public Functions
 */


/* Version of x86 trace producer.
 * See 'src/visual/x86/cpu.c' for x86 trace consumer. */

#define X86_TRACE_VERSION_MAJOR		1
#define X86_TRACE_VERSION_MINOR		671


void x86_cpu_read_config(void)
{
	struct config_t *config;
	char *section;

	/* Open file */
	config = config_create(x86_config_file_name);
	if (*x86_config_file_name)
		config_load(config);


	/* General configuration */

	section = "General";

	arch_x86->frequency = config_read_int(config, section, "Frequency", 3000);
	if (!IN_RANGE(arch_x86->frequency, 1, ESIM_MAX_FREQUENCY))
		fatal("%s: invalid value for 'Frequency'.", x86_config_file_name);

	x86_cpu_num_cores = config_read_int(config, section, "Cores", x86_cpu_num_cores);
	x86_cpu_num_threads = config_read_int(config, section, "Threads", x86_cpu_num_threads);

	if (x86_cpu_fast_forward_count <= 0)
		x86_cpu_fast_forward_count = config_read_llint(config, section, "FastForward", 0);

	x86_cpu_context_quantum = config_read_int(config, section, "ContextQuantum", 100000);
	x86_cpu_thread_quantum = config_read_int(config, section, "ThreadQuantum", 1000);
	x86_cpu_thread_switch_penalty = config_read_int(config, section, "ThreadSwitchPenalty", 0);

	x86_cpu_recover_kind = config_read_enum(config, section, "RecoverKind", x86_cpu_recover_kind_writeback, x86_cpu_recover_kind_map, 2);
	x86_cpu_recover_penalty = config_read_int(config, section, "RecoverPenalty", 0);

	x86_emu_process_prefetch_hints = config_read_bool(config, section, "ProcessPrefetchHints", 1);
	prefetch_history_size = config_read_int(config, section, "PrefetchHistorySize", 10);

	/* Create cpu and cores for storing the configuration */
	x86_cpu = xcalloc(1, sizeof(struct x86_cpu_t));
	x86_cpu->core = xcalloc(x86_cpu_num_cores, sizeof(struct x86_core_t));
	x86_cpu->num_cores = x86_cpu_num_cores;

	/* Section '[ Pipeline ]' */

	section = "Pipeline";

	x86_cpu_fetch_kind = config_read_enum(config, section, "FetchKind", x86_cpu_fetch_kind_timeslice, x86_cpu_fetch_kind_map, 3);

	x86_cpu_decode_width = config_read_int(config, section, "DecodeWidth", 4);

	x86_cpu_dispatch_kind = config_read_enum(config, section, "DispatchKind", x86_cpu_dispatch_kind_timeslice, x86_cpu_dispatch_kind_map, 2);
	x86_cpu_dispatch_width = config_read_int(config, section, "DispatchWidth", 4);

	x86_cpu_issue_kind = config_read_enum(config, section, "IssueKind", x86_cpu_issue_kind_timeslice, x86_cpu_issue_kind_map, 2);
	x86_cpu_issue_width = config_read_int(config, section, "IssueWidth", 4);

	x86_cpu_commit_kind = config_read_enum(config, section, "CommitKind", x86_cpu_commit_kind_shared, x86_cpu_commit_kind_map, 2);
	x86_cpu_commit_width = config_read_int(config, section, "CommitWidth", 4);

	x86_cpu_occupancy_stats = config_read_bool(config, section, "OccupancyStats", 0);


	/* Section '[ Queues ]' */

	section = "Queues";

	x86_fetch_queue_size = config_read_int(config, section, "FetchQueueSize", 64);

	x86_uop_queue_size = config_read_int(config, section, "UopQueueSize", 32);

	x86_rob_kind = config_read_enum(config, section, "RobKind", x86_rob_kind_private, x86_rob_kind_map, 2);
	x86_rob_size = config_read_int(config, section, "RobSize", 64);

	x86_iq_kind = config_read_enum(config, section, "IqKind", x86_iq_kind_private, x86_iq_kind_map, 2);
	x86_iq_size = config_read_int(config, section, "IqSize", 40);

	x86_lsq_kind = config_read_enum(config, section, "LsqKind", x86_lsq_kind_private, x86_lsq_kind_map, 2);
	x86_lq_size = config_read_int(config, section, "LqSize", 32);
	x86_sq_size = config_read_int(config, section, "SqSize", 32);
	x86_pq_size = config_read_int(config, section, "PqSize", 32);

	x86_reg_file_kind = config_read_enum(config, section, "RfKind", x86_reg_file_kind_private, x86_reg_file_kind_map, 2);
	x86_reg_file_int_size = config_read_int(config, section, "RfIntSize", 80);
	x86_reg_file_fp_size = config_read_int(config, section, "RfFpSize", 40);
	x86_reg_file_xmm_size = config_read_int(config, section, "RfXmmSize", 40);


	/* Functional Units */
	x86_fu_read_config(config);


	/* Branch Predictor */

	section = "BranchPredictor";

	x86_bpred_kind = config_read_enum(config, section, "Kind", x86_bpred_kind_twolevel, x86_bpred_kind_map, 6);
	x86_bpred_btb_sets = config_read_int(config, section, "BTB.Sets", 256);
	x86_bpred_btb_assoc = config_read_int(config, section, "BTB.Assoc", 4);
	x86_bpred_bimod_size = config_read_int(config, section, "Bimod.Size", 1024);
	x86_bpred_choice_size = config_read_int(config, section, "Choice.Size", 1024);
	x86_bpred_ras_size = config_read_int(config, section, "RAS.Size", 32);
	x86_bpred_twolevel_l1size = config_read_int(config, section, "TwoLevel.L1Size", 1);
	x86_bpred_twolevel_l2size = config_read_int(config, section, "TwoLevel.L2Size", 1024);
	x86_bpred_twolevel_hist_size = config_read_int(config, section, "TwoLevel.HistorySize", 8);

	/* Trace Cache */
	x86_trace_cache_read_config(config);

	/* Check parameters */
	if (x86_cpu_num_cores < 1 && x86_cpu_num_cores > 128)
		fatal("%s: Number of cores must be > 1 and <= 128.\n", __FUNCTION__);

	if (x86_cpu_num_threads < 1 && x86_cpu_num_threads > 128)
		fatal("%s: Number of threads per core must be > 1 and <= 128.\n", __FUNCTION__);

	/* Close file */
	config_check(config);
	config_free(config);
}


void x86_cpu_init(void)
{
	int core;

	/* Trace */
	x86_trace_category = trace_new_category();

	/* Initialize */
	x86_cpu->uop_trace_list = linked_list_create();

	/* Initialize cores */
	X86_CORE_FOR_EACH
		x86_cpu_core_init(core);

	/* Components of an x86 CPU */
	x86_reg_file_init();
	x86_bpred_init();
	x86_trace_cache_init();
	x86_fetch_queue_init();
	x86_uop_queue_init();
	x86_rob_init();
	x86_iq_init();
	x86_lsq_init();
	x86_event_queue_init();
	x86_fu_init();

	/* Trace */
	x86_trace_header("x86.init version=\"%d.%d\" num_cores=%d num_threads=%d\n",
		X86_TRACE_VERSION_MAJOR, X86_TRACE_VERSION_MINOR,
		x86_cpu_num_cores, x86_cpu_num_threads);
}


/* Finalization */
void x86_cpu_done(void)
{
	int core;

	/* Dump CPU report */
	x86_cpu_dump_report();

	/* Uop trace list */
	x86_cpu_uop_trace_list_empty();
	linked_list_free(x86_cpu->uop_trace_list);

	/* Finalize structures */
	x86_fetch_queue_done();
	x86_uop_queue_done();
	x86_rob_done();
	x86_iq_done();
	x86_lsq_done();
	x86_event_queue_done();
	x86_bpred_done();
	x86_trace_cache_done();
	x86_reg_file_done();
	x86_fu_done();

	/* Free processor */
	X86_CORE_FOR_EACH
		x86_cpu_core_done(core);
	free(x86_cpu->core);
	free(x86_cpu);
}


void x86_cpu_dump(FILE *f)
{
	int core;
	int thread;

	/* General information */
	fprintf(f, "\n");
	fprintf(f, "LastDump = %lld   ; Cycle of last dump\n", x86_cpu->last_dump);
	fprintf(f, "IPCLastDump = %.4g   ; IPC since last dump\n",
			arch_x86->cycle - x86_cpu->last_dump > 0 ?
			(double) (x86_cpu->num_committed_uinst - x86_cpu->last_committed)
			/ (arch_x86->cycle - x86_cpu->last_dump) : 0);
	fprintf(f, "\n");

	/* Cores */
	X86_CORE_FOR_EACH
	{
		fprintf(f, "-------\n");
		fprintf(f, "Core %d\n", core);
		fprintf(f, "-------\n\n");

		fprintf(f, "Event Queue:\n");
		x86_uop_linked_list_dump(X86_CORE.event_queue, f);

		fprintf(f, "Reorder Buffer:\n");
		x86_rob_dump(core, f);


		X86_THREAD_FOR_EACH
		{
			fprintf(f, "----------------------\n");
			fprintf(f, "Thread %d (in core %d)\n", thread, core);
			fprintf(f, "----------------------\n\n");

			fprintf(f, "Fetch queue:\n");
			x86_uop_list_dump(X86_THREAD.fetch_queue, f);

			fprintf(f, "Uop queue:\n");
			x86_uop_list_dump(X86_THREAD.uop_queue, f);

			fprintf(f, "Instruction Queue:\n");
			x86_uop_linked_list_dump(X86_THREAD.iq, f);

			fprintf(f, "Load Queue:\n");
			x86_uop_linked_list_dump(X86_THREAD.lq, f);

			fprintf(f, "Store Queue:\n");
			x86_uop_linked_list_dump(X86_THREAD.sq, f);

			x86_reg_file_dump(core, thread, f);
			if (X86_THREAD.ctx)
				fprintf(f, "MappedContext = %d\n", X86_THREAD.ctx->pid);

			fprintf(f, "\n");
		}
	}

	/* Register last dump */
	x86_cpu->last_dump = arch_x86->cycle;
	x86_cpu->last_committed = x86_cpu->num_committed_uinst;

	/* End */
	fprintf(f, "\n\n");
}


void x86_cpu_dump_summary(FILE *f)
{
	double inst_per_cycle;
	double uinst_per_cycle;
	double branch_acc;

	/* Calculate statistics */
	inst_per_cycle = arch_x86->cycle ? (double) x86_cpu->num_committed_inst / (arch_x86->cycle - arch_x86->last_reset_cycle) : 0.0;
	uinst_per_cycle = arch_x86->cycle ? (double) x86_cpu->num_committed_uinst / (arch_x86->cycle - arch_x86->last_reset_cycle) : 0.0;
	branch_acc = x86_cpu->num_branch_uinst ? (double) (x86_cpu->num_branch_uinst - x86_cpu->num_mispred_branch_uinst) / x86_cpu->num_branch_uinst : 0.0;

	/* Print statistics */
	fprintf(f, "FastForwardInstructions = %lld\n", x86_cpu->num_fast_forward_inst);
	fprintf(f, "CommittedInstructions = %lld\n", x86_cpu->num_committed_inst);
	fprintf(f, "CommittedInstructionsPerCycle = %.4g\n", inst_per_cycle);
	fprintf(f, "CommittedMicroInstructions = %lld\n", x86_cpu->num_committed_uinst);
	fprintf(f, "CommittedMicroInstructionsPerCycle = %.4g\n", uinst_per_cycle);
	fprintf(f, "BranchPredictionAccuracy = %.4g\n", branch_acc);
}


#define UPDATE_THREAD_OCCUPANCY_STATS(ITEM) { \
	X86_THREAD.ITEM##_occupancy += X86_THREAD.ITEM##_count; \
	if (X86_THREAD.ITEM##_count == x86_##ITEM##_size) \
		X86_THREAD.ITEM##_full++; \
}


#define UPDATE_CORE_OCCUPANCY_STATS(ITEM) { \
	X86_CORE.ITEM##_occupancy += X86_CORE.ITEM##_count; \
	if (X86_CORE.ITEM##_count == x86_##ITEM##_size * x86_cpu_num_threads) \
		X86_CORE.ITEM##_full++; \
}


void x86_cpu_update_occupancy_stats(void)
{
	int core, thread;

	X86_CORE_FOR_EACH
	{
		/* Update occupancy stats for shared structures */
		if (x86_rob_kind == x86_rob_kind_shared)
			UPDATE_CORE_OCCUPANCY_STATS(rob);
		if (x86_iq_kind == x86_iq_kind_shared)
			UPDATE_CORE_OCCUPANCY_STATS(iq);
		if (x86_lsq_kind == x86_lsq_kind_shared)
		{
			UPDATE_CORE_OCCUPANCY_STATS(lq);
			UPDATE_CORE_OCCUPANCY_STATS(sq);
			UPDATE_CORE_OCCUPANCY_STATS(pq);
		}
		if (x86_reg_file_kind == x86_reg_file_kind_shared)
		{
			UPDATE_CORE_OCCUPANCY_STATS(reg_file_int);
			UPDATE_CORE_OCCUPANCY_STATS(reg_file_fp);
		}

		/* Occupancy stats for private structures */
		X86_THREAD_FOR_EACH
		{
			if (x86_rob_kind == x86_rob_kind_private)
				UPDATE_THREAD_OCCUPANCY_STATS(rob);
			if (x86_iq_kind == x86_iq_kind_private)
				UPDATE_THREAD_OCCUPANCY_STATS(iq);
			if (x86_lsq_kind == x86_lsq_kind_private)
			{
				UPDATE_THREAD_OCCUPANCY_STATS(lq);
				UPDATE_THREAD_OCCUPANCY_STATS(sq);
				UPDATE_THREAD_OCCUPANCY_STATS(pq);
			}
			if (x86_reg_file_kind == x86_reg_file_kind_private)
			{
				UPDATE_THREAD_OCCUPANCY_STATS(reg_file_int);
				UPDATE_THREAD_OCCUPANCY_STATS(reg_file_fp);
			}
		}
	}
}


void x86_cpu_uop_trace_list_add(struct x86_uop_t *uop)
{
	assert(x86_tracing());
	assert(!uop->in_uop_trace_list);

	uop->in_uop_trace_list = 1;
	linked_list_add(x86_cpu->uop_trace_list, uop);
}


void x86_cpu_uop_trace_list_empty(void)
{
	struct linked_list_t *uop_trace_list;
	struct x86_uop_t *uop;

	uop_trace_list = x86_cpu->uop_trace_list;
	while (uop_trace_list->count)
	{
		/* Remove from list */
		linked_list_head(uop_trace_list);
		uop = linked_list_get(uop_trace_list);
		linked_list_remove(uop_trace_list);
		assert(uop->in_uop_trace_list);

		/* Trace */
		x86_trace("x86.end_inst id=%lld core=%d\n",
			uop->id_in_core, uop->core);

		/* Free uop */
		uop->in_uop_trace_list = 0;
		x86_uop_free_if_not_queued(uop);
	}
}


void x86_cpu_run_stages(void)
{
	/* Context scheduler */
	x86_cpu_schedule();

	/* Stages */
	x86_cpu_commit();
	x86_cpu_writeback();
	x86_cpu_issue();
	x86_cpu_dispatch();
	x86_cpu_decode();
	x86_cpu_fetch();

	/* Update stats for structures occupancy */
	if (x86_cpu_occupancy_stats)
		x86_cpu_update_occupancy_stats();
}


/* Run fast-forward simulation */
void x86_cpu_run_fast_forward(void)
{
	/* Fast-forward simulation. Run 'x86_cpu_fast_forward' iterations of the x86
	 * emulation loop until any simulation end reason is detected. */
	while (arch_x86->inst_count < x86_cpu_fast_forward_count && !esim_finish)
		x86_emu_run();

	/* Record number of instructions in fast-forward execution. */
	x86_cpu->num_fast_forward_inst = arch_x86->inst_count;

	/* Output warning if simulation finished during fast-forward execution. */
	if (esim_finish)
		warning("x86 fast-forwarding finished simulation.\n%s",
				x86_cpu_err_fast_forward);
}


/* Run one iteration of timing simulation. Return TRUE if still running. */
int x86_cpu_run(void)
{
	struct x86_ctx_t *ctx;

	/* Finish contexts that have surpassed their target number of instructions */
	for (ctx = x86_emu->context_list_head; ctx; ctx = ctx->context_list_next)
		if (!x86_ctx_get_state(ctx, x86_ctx_finished | x86_ctx_zombie) &&
				ctx->max_instructions &&
				ctx->num_committed_inst > ctx->max_instructions)
			x86_ctx_finish(ctx, -1);

	/* Stop if no context is running */
	if (x86_emu->finished_list_count >= x86_emu->context_list_count)
		return FALSE;

	/* Fast-forward simulation */
	if (x86_cpu_fast_forward_count && arch_x86->inst_count < x86_cpu_fast_forward_count)
	{
		x86_emu_max_inst += x86_cpu_fast_forward_count;
		x86_emu_min_inst_per_ctx += x86_cpu_fast_forward_count;
		x86_cpu_run_fast_forward();
		x86_emu_max_inst -= x86_cpu_fast_forward_count;
		x86_emu_min_inst_per_ctx -= x86_cpu_fast_forward_count;
	}

	/* Stop if maximum number of CPU instructions exceeded */
	if (x86_emu_max_inst && x86_cpu->num_committed_inst >= x86_emu_max_inst)
		esim_finish = esim_finish_x86_max_inst;

	/* Stop if maximum number of cycles exceeded */
	if (x86_emu_max_cycles && arch_x86->cycle >= x86_emu_max_cycles)
		esim_finish = esim_finish_x86_max_cycles;

	/* Stop if minimum number of instructions has been exceeded by all contexts */
	if(x86_emu_min_inst_per_ctx)
	{
		for (ctx = x86_emu->context_list_head; ctx; ctx = ctx->context_list_next)
			if (!x86_ctx_get_state(ctx, x86_ctx_finished | x86_ctx_zombie) && ctx->num_committed_inst < x86_emu_min_inst_per_ctx)
				break;
		if(!ctx)
			esim_finish = esim_finish_x86_min_inst_per_ctx;
	}

	/* Reset stats if minimum number of instructions has been exceeded by all contexts */
	if(x86_cpu_warm_up_count && !arch_x86->last_reset_cycle)
	{
		for (ctx = x86_emu->running_list_head; ctx; ctx = ctx->running_list_next)
			if(ctx->num_committed_inst < x86_cpu_warm_up_count)
				break;
		if(!ctx)
		{
			x86_cpu_reset_stats();
			if (x86_emu_min_inst_per_ctx)
				x86_emu_min_inst_per_ctx -= x86_cpu_warm_up_count;
			if (x86_save_checkpoint_after_warm_up_file_name)
				x86_checkpoint_save(x86_save_checkpoint_after_warm_up_file_name);
		}
	}

	/* Stop if any previous reason met */
	if (esim_finish)
		return TRUE;

	/* One more cycle of x86 timing simulation */
	arch_x86->cycle++;

	/* Empty uop trace list. This dumps the last trace line for instructions
	 * that were freed in the previous simulation cycle. */
	x86_cpu_uop_trace_list_empty();

	/* Processor stages */
	x86_cpu_run_stages();

	/* Process host threads generating events */
	x86_emu_process_events();

	/* Still simulating */
	return TRUE;
}


void x86_thread_report_stack_reset_stats(struct x86_thread_report_stack_t *stack)
{
	if (!stack)
		return;

	/* stack->last_cycle = esim_cycle(); */
	/* stack->dispatch_stall_cycles_rob_mem = 0; */
	/* stack->dispatch_stall_cycles_rob_load = 0; */
    /*  */
	/* #<{(| Erase report file |)}># */
	/* fseeko(f, 0, SEEK_SET); */
    /*  */
	/* Print header */
	/* TODO */
}


void x86_thread_reset_stats(int core, int thread)
{
}


void x86_core_reset_stats(int core)
{
}


void x86_cpu_reset_stats(void)
{
	int i;
	int core;
	int thread;

	/* TODO: Integrate with other archs */
	/* TODO: Reset network stats when all archs are reseted */
	/* TODO: Reset dramsim stats? */

	/* Reset cores, threads and associated modules */
	X86_CORE_FOR_EACH
	{
		X86_THREAD_FOR_EACH
			x86_thread_reset_stats(core, thread);
		x86_core_reset_stats(core);
	}

	x86_cpu->num_fetched_uinst = 0;
	for (i = 0; i < x86_uinst_opcode_count; i++)
	{
		x86_cpu->num_dispatched_uinst_array[i] = 0;
		x86_cpu->num_issued_uinst_array[i] = 0;
		x86_cpu->num_committed_uinst_array[i] = 0;
	}
	x86_cpu->num_committed_uinst = 0; /* Committed micro-instructions */
	x86_cpu->num_committed_inst = 0; /* Committed x86 instructions */
	x86_cpu->num_squashed_uinst = 0;
	x86_cpu->num_branch_uinst = 0;
	x86_cpu->num_mispred_branch_uinst = 0;

	/* Reset x86 ctxs stats */
	x86_ctx_all_reset_stats();

	/* Register last reset */
	arch_x86->last_reset_cycle = arch_x86->cycle;
}


static void x86_cpu_thread_interval_report_init(int core, int thread)
{
	struct x86_thread_report_stack_t *stack;
	char interval_report_file_name[MAX_PATH_SIZE];
	int ret;

	/* Create new stack */
	stack = xcalloc(1, sizeof(struct x86_thread_report_stack_t));
	stack->hits_per_level_int = xcalloc(max_mod_level + 1, sizeof(long long)); /* As there isn't any level 0, position 0 in the array is not used */
	stack->stream_hits_per_level_int = xcalloc(max_mod_level + 1, sizeof(long long));
	stack->misses_per_level_int = xcalloc(max_mod_level + 1, sizeof(long long));
	stack->retries_per_level_int = xcalloc(max_mod_level + 1, sizeof(long long));
	stack->evictions_per_level_int = xcalloc(max_mod_level + 1, sizeof(long long));

	/* Interval reporting of stats */
	ret = snprintf(interval_report_file_name, MAX_PATH_SIZE, "%s/c%dt%d.intrep.csv", x86_thread_interval_reports_dir, core, thread);
	if (ret < 0 || ret >= MAX_PATH_SIZE)
		fatal("warning: function %s: string too long %s", __FUNCTION__, interval_report_file_name);

	stack->report_file = file_open_for_write(interval_report_file_name);
	if (!stack->report_file)
		fatal("%s: cannot open interval report file", interval_report_file_name);

	stack->core = core;
	stack->thread = thread;

	X86_THREAD.report_stack = stack;

	fprintf(stack->report_file, "%s", "esim-time");                                        /* Global simulation time */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "inst");                       /* Instructions */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "uinst");                      /* Microinstructions */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "ipc-int");                    /* Microinstructions per cycle in the interval */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "ipc-alone-int");              /* Estimation of IPC in the interval if the context would be executing alone */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "ipc-glob");                   /* Global microinstructions per cycle */
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "ipc-alone-glob");             /* Estimation of the IPC if the context would be executiong alone */
	for (int i = 0; i < x86_dispatch_stall_max; i++)                                       /* Where the dispatch slots are going */
		fprintf(stack->report_file, ",c%dt%d-%s", core, thread, str_map_value(&x86_dispatch_stall_map, i));
	fprintf(stack->report_file, ",c%dt%d-%s", core, thread, "interthread-penalty-int");    /* Number of cycles with the ROB stalled dua a interthread miss */
	for (int level = 1; level <= max_mod_level - 1; level++)
	{
		fprintf(stack->report_file, ",c%dt%d-l%d-%s", core, thread, level, "hits-int");
		fprintf(stack->report_file, ",c%dt%d-l%d-%s", core, thread, level, "stream-hits-int");
		fprintf(stack->report_file, ",c%dt%d-l%d-%s", core, thread, level, "misses-int");
		fprintf(stack->report_file, ",c%dt%d-l%d-%s", core, thread, level, "retries-int");
		fprintf(stack->report_file, ",c%dt%d-l%d-%s", core, thread, level, "evictions-int");
	}
	fprintf(stack->report_file, "\n");
	fflush(stack->report_file);
}


static void x86_cpu_thread_interval_report(int core, int thread)
{
	struct x86_thread_report_stack_t *stack = X86_THREAD.report_stack;
	long long cycles_int = arch_x86->cycle - stack->last_cycle;
	long long num_committed_uinst_int;
	double ipc_int;
	double ipc_glob;
	double ipc_alone_int;
	double ipc_alone_glob;
	double interthread_penalty_cycles_int;
	double dispatch_total_slots = 0;
	double dispatch_stall_int[x86_dispatch_stall_max];

	/* Ratio of usage and stall of dispatch slots */
	for (int i = 0; i < x86_dispatch_stall_max; i++)
	{
		dispatch_stall_int[i] = X86_THREAD.dispatch_stall[i] - stack->dispatch_stall[i];
		dispatch_total_slots += dispatch_stall_int[i];
	}
	for (int i = 0; i < x86_dispatch_stall_max; i++)
	{
		dispatch_stall_int[i] = dispatch_total_slots > 0 ?
				dispatch_stall_int[i] / dispatch_total_slots :
				NAN;
	}

	interthread_penalty_cycles_int = X86_THREAD.interthread_penalty_cycles - stack->interthread_penalty_cycles;

	num_committed_uinst_int = X86_THREAD.num_committed_uinst - stack->num_committed_uinst;
	ipc_glob = arch_x86->cycle - arch_x86->last_reset_cycle ?
			(double) X86_THREAD.num_committed_uinst / (arch_x86->cycle - arch_x86->last_reset_cycle) : 0.0;
	ipc_int = (double) num_committed_uinst_int / cycles_int;

	ipc_alone_glob = arch_x86->cycle - X86_THREAD.interthread_penalty_cycles - arch_x86->last_reset_cycle ?
			(double) X86_THREAD.num_committed_uinst / (arch_x86->cycle - X86_THREAD.interthread_penalty_cycles - arch_x86->last_reset_cycle) : 0.0;
	ipc_alone_int = (double) num_committed_uinst_int / (cycles_int - interthread_penalty_cycles_int);

	fprintf(stack->report_file, "%lld", esim_time);
	fprintf(stack->report_file, ",%lld", X86_THREAD.num_committed_inst);
	fprintf(stack->report_file, ",%lld", X86_THREAD.num_committed_uinst);
	fprintf(stack->report_file, ",%.3f", ipc_int);
	fprintf(stack->report_file, ",%.3f", ipc_alone_int);
	fprintf(stack->report_file, ",%.3f", ipc_glob);
	fprintf(stack->report_file, ",%.3f", ipc_alone_glob);
	for (int i = 0; i < x86_dispatch_stall_max; i++)
		fprintf(stack->report_file, ",%.3f", dispatch_stall_int[i]);
	fprintf(stack->report_file, ",%.3f", interthread_penalty_cycles_int);
	for (int level = 1; level <= max_mod_level - 1; level++)
	{
		fprintf(stack->report_file, ",%lld", stack->hits_per_level_int[level]);
		fprintf(stack->report_file, ",%lld", stack->stream_hits_per_level_int[level]);
		fprintf(stack->report_file, ",%lld", stack->misses_per_level_int[level]);
		fprintf(stack->report_file, ",%lld", stack->retries_per_level_int[level]);
		fprintf(stack->report_file, ",%lld", stack->evictions_per_level_int[level]);
	}
	fprintf(stack->report_file, "\n");
	fflush(stack->report_file);

	/* Update intermediate results */
	stack->last_cycle = arch_x86->cycle;
	for (int i = 0; i < x86_dispatch_stall_max; i++)
		stack->dispatch_stall[i] = X86_THREAD.dispatch_stall[i];
	stack->num_committed_uinst = X86_THREAD.num_committed_uinst;
	stack->interthread_penalty_cycles = X86_THREAD.interthread_penalty_cycles;
	for (int level = 1; level <= max_mod_level - 1; level++) /* Deepest mod level is main memory, not cache */
	{
		stack->hits_per_level_int[level] = 0;
		stack->stream_hits_per_level_int[level] = 0;
		stack->misses_per_level_int[level] = 0;
		stack->retries_per_level_int[level] = 0;
		stack->evictions_per_level_int[level] = 0;
	}
}


void x86_cpu_interval_report_init()
{
	int core;
	int thread;

	X86_CORE_FOR_EACH
		X86_THREAD_FOR_EACH
			x86_cpu_thread_interval_report_init(core, thread);
}


void x86_cpu_interval_report()
{
	int core;
	int thread;

	X86_CORE_FOR_EACH
		X86_THREAD_FOR_EACH
			x86_cpu_thread_interval_report(core, thread);
}


static void x86_cpu_thread_mapping_report_init(int core, int thread)
{
	char report_file_name[MAX_PATH_SIZE];
	int ret;

	/* Interval reporting of stats */
	ret = snprintf(report_file_name, MAX_PATH_SIZE, "%s/c%dt%d.maprep.csv", x86_thread_mappings_reports_dir, core, thread);
	if (ret < 0 || ret >= MAX_PATH_SIZE)
		fatal("warning: function %s: string too long %s", __FUNCTION__, report_file_name);

	X86_THREAD.mapping_report_file = file_open_for_write(report_file_name);
	if (!X86_THREAD.mapping_report_file)
		fatal("%s: cannot open interval report file", report_file_name);

	/* Print header */
	fprintf(X86_THREAD.mapping_report_file, "%s", "esim-time");
	fprintf(X86_THREAD.mapping_report_file, ",c%dt%d-%s", core, thread, "allocated");
	fprintf(X86_THREAD.mapping_report_file, "\n");
	fflush(X86_THREAD.mapping_report_file);
}
