/*
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
#include <string.h>

#include <arch/common/arch.h>
#include <arch/southern-islands/timing/gpu.h>
#include <arch/x86/timing/cpu.h>
#include <dramsim/bindings-c.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/bloom.h>
#include <lib/util/debug.h>
#include <lib/util/file.h>
#include <lib/util/hash-table.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>
#include <lib/util/misc.h>
#include <lib/util/stats.h>
#include <lib/util/string.h>
#include <network/net-system.h>
#include <network/network.h>
#include <network/node.h>
#include <network/routing-table.h>

#include "atd.h"
#include "cache.h"
#include "cache-partitioning.h"
#include "command.h"
#include "directory.h"
#include "fcp.h"
#include "mem-system.h"
#include "mmu.h"
#include "module.h"
#include "prefetcher.h"
#include "ucp.h"


/*
 * Global Variables
 */
//Hugo
struct mod_t *mod_RTM = NULL;
char *mem_config_file_name = "";

char *mem_config_help =
	"Option '--mem-config <file>' is used to configure the memory system. The\n"
	"configuration file is a plain-text file in the IniFile format. The memory\n"
	"system is formed of a set of cache modules, main memory modules, and\n"
	"interconnects.\n"
	"\n"
	"Interconnects can be defined in two different configuration files. The first\n"
	"way is using option '--net-config <file>' (use option '--help-net-config'\n"
	"for more information). Any network defined in the network configuration file\n"
	"can be referenced from the memory configuration file. These networks will be\n"
	"referred hereafter as external networks.\n"
	"\n"
	"The second option to define a network straight in the memory system\n"
	"configuration. This alternative is provided for convenience and brevity. By\n"
	"using sections [Network <name>], networks with a default topology are\n"
	"created which include a single switch, and one bidirectional link from the\n"
	"switch to every end node present in the network.\n"
	"\n"
	"The following sections and variables can be used in the memory system\n"
	"configuration file:\n"
	"\n"
	"Section [General] defines global parameters affecting the entire memory\n"
	"system.\n"
	"\n"
	"  Frequency = <value>  (Default = 1000)\n"
	"      Frequency of the memory system in MHz.\n"
	"  PageSize = <size>  (Default = 4096)\n"
	"      Memory page size. Virtual addresses are translated into new physical\n"
	"      addresses in ascending order at the granularity of the page size.\n"
	"  PeerTransfers = <bool> (Default = transfers)\n"
	"      Whether or not transfers between peer caches are used.\n"
	"\n"
	"Section [Module <name>] defines a generic memory module. This section is\n"
	"used to declare both caches and main memory modules accessible from CPU\n"
	"cores or GPU compute units.\n"
	"\n"
	"  Type = {Cache|MainMemory}  (Required)\n"
	"      Type of the memory module. From the simulation point of view, the\n"
	"      difference between a cache and a main memory module is that the former\n"
	"      contains only a subset of the data located at the memory locations it\n"
	"      serves.\n"
	"  Geometry = <geo>\n"
	"      Cache geometry, defined in a separate section of type\n"
	"      [Geometry <geo>]. This variable is required for cache modules.\n"
	"  LowNetwork = <net>\n"
	"      Network connecting the module with other lower-level modules, i.e.,\n"
	"      modules closer to main memory. This variable is mandatory for caches,\n"
	"      and should not appear for main memory modules. Value <net> can refer\n"
	"      to an internal network defined in a [Network <net>] section, or to an\n"
	"      external network defined in the network configuration file.\n"
	"  LowNetworkNode = <node>\n"
	"      If 'LowNetwork' points to an external network, node in the network\n"
	"      that the module is mapped to. For internal networks, this variable\n"
	"      should be omitted.\n"
	"  HighNetwork = <net>\n"
	"      Network connecting the module with other higher-level modules, i.e.,\n"
	"      modules closer to CPU cores or GPU compute units. For highest level\n"
	"      modules accessible by CPU/GPU, this variable should be omitted.\n"
	"  HighNetworkNode = <node>\n"
	"      If 'HighNetwork' points to an external network, node that the module\n"
	"      is mapped to.\n"
	"  LowModules = <mod1> [<mod2> ...]\n"
	"      List of lower-level modules. For a cache module, this variable is\n"
	"      required. If there is only one lower-level module, it serves the\n"
	"      entire address space for the current module. If there are several\n"
	"      lower-level modules, each served a disjoint subset of the address\n"
	"      space. This variable should be omitted for main memory modules.\n"
	"  BlockSize = <size>\n"
	"      Block size in bytes. This variable is required for a main memory\n"
	"      module. It should be omitted for a cache module (in this case, the\n"
	"      block size is specified in the corresponding cache geometry section).\n"
	"  Latency = <cycles>\n"
	"      Memory access latency. This variable is required for a main memory\n"
	"      module, and should be omitted for a cache module (the access latency\n"
	"      is specified in the corresponding cache geometry section).\n"
	"  Ports = <num>\n"
	"      Number of read/write ports. This variable is only allowed for a main\n"
	"      memory module. The number of ports for a cache is specified in a\n"
	"      separate cache geometry section.\n"
	"  DirectorySize <size>\n"
	"      Size of the directory in number of blocks. The size of a directory\n"
	"      limits the number of different blocks that can reside in upper-level\n"
	"      caches. If a cache requests a new block from main memory, and its\n"
	"      directory is full, a previous block must be evicted from the\n"
	"      directory, and all its occurrences in the memory hierarchy need to be\n"
	"      first invalidated. This variable is only allowed for a main memory\n"
	"      module.\n"
	"  DirectoryAssoc = <assoc>\n"
	"      Directory associativity in number of ways. This variable is only\n"
	"      allowed for a main memory module.\n"
	"  AddressRange = { BOUNDS <low> <high> | ADDR DIV <div> MOD <mod> EQ <eq> }\n"
	"      Physical address range served by the module. If not specified, the\n"
	"      entire address space is served by the module. There are two possible\n"
	"      formats for the value of 'Range':\n"
	"      With the first format, the user can specify the lowest and highest\n"
	"      byte included in the address range. The value in <low> must be a\n"
	"      multiple of the module block size, and the value in <high> must be a\n"
	"      multiple of the block size minus 1.\n"
	"      With the second format, the address space can be split between\n"
	"      different modules in an interleaved manner. If dividing an address\n"
	"      by <div> and modulo <mod> makes it equal to <eq>, it is served by\n"
	"      this module. The value of <div> must be a multiple of the block size.\n"
	"      When a module serves only a subset of the address space, the user must\n"
	"      make sure that the rest of the modules at the same level serve the\n"
	"      remaining address space.\n"
	"\n"
	"Section [CacheGeometry <geo>] defines a geometry for a cache. Caches using\n"
	"this geometry are instantiated [Module <name>] sections.\n"
	"\n"
	"  Sets = <num_sets> (Required)\n"
	"      Number of sets in the cache.\n"
	"  Assoc = <num_ways> (Required)\n"
	"      Cache associativity. The total number of blocks contained in the cache\n"
	"      is given by the product Sets * Assoc.\n"
	"  BlockSize = <size> (Required)\n"
	"      Size of a cache block in bytes. The total size of the cache is given\n"
	"      by the product Sets * Assoc * BlockSize.\n"
	"  Latency = <cycles> (Required)\n"
	"      Hit latency for a cache in number of cycles.\n"
	"  Policy = {LRU|FIFO|Random} (Default = LRU)\n"
	"      Block replacement policy.\n"
	"  MSHR = <size> (Default = 16)\n"
	"      Miss status holding register (MSHR) size in number of entries. This\n"
	"      value determines the maximum number of accesses that can be in flight\n"
	"      for the cache, including the time since the access request is\n"
	"      received, until a potential miss is resolved.\n"
	"  Ports = <num> (Default = 2)\n"
	"      Number of ports. The number of ports in a cache limits the number of\n"
	"      concurrent hits. If an access is a miss, it remains in the MSHR while\n"
	"      it is resolved, but releases the cache port.\n"
	"  DirectoryLatency = <cycles> (Default = 1)\n"
	"      Latency for a directory access in number of cycles.\n"
	"  EnablePrefetcher = {t|f} (Default = False)\n"
	"      Whether the hardware should automatically perform prefetching.\n"
	"      The prefetcher related options below will be ignored if this is\n"
	"      not true.\n"
	"  PrefetcherType = {GHB_PC_CS|GHB_PC_DC} (Default GHB_PC_CS)\n"
	"      Specify the type of global history buffer based prefetcher to use.\n"
	"      GHB_PC_CS - Program Counter indexed, Constant Stride.\n"
	"      GHB_PC_DC - Program Counter indexed, Delta Correlation.\n"
	"  PrefetcherGHBSize = <size> (Default = 256)\n"
	"      The hardware prefetcher does global history buffer based prefetching.\n"
	"      This option specifies the size of the global history buffer.\n"
	"  PrefetcherITSize = <size> (Default = 64)\n"
	"      The hardware prefetcher does global history buffer based prefetching.\n"
	"      This option specifies the size of the index table used.\n"
	"  PrefetcherLookupDepth = <num> (Default = 2)\n"
	"      This option specifies the history (pattern) depth upto which the\n"
	"      prefetcher looks at the history to decide when to prefetch.\n"
	"\n"
	"Section [Network <net>] defines an internal default interconnect, formed of\n"
	"a single switch connecting all modules pointing to the network. For every\n"
	"module in the network, a bidirectional link is created automatically between\n"
	"the module and the switch, together with the suitable input/output buffers\n"
	"in the switch and the module.\n"
	"\n"
	"  DefaultInputBufferSize = <size>\n"
	"      Size of input buffers for end nodes (memory modules) and switch.\n"
	"  DefaultOutputBufferSize = <size>\n"
	"      Size of output buffers for end nodes and switch. \n"
	"  DefaultBandwidth = <bandwidth>\n"
	"      Bandwidth for links and switch crossbar in number of bytes per cycle.\n"
	"\n"
	"Section [Entry <name>] creates an entry into the memory system. An entry is\n"
	"a connection between a CPU core/thread or a GPU compute unit with a module\n"
	"in the memory system.\n"
	"\n"
	"  Arch = { x86 | Evergreen | SouthernIslands | ... }\n"
	"      CPU or GPU architecture affected by this entry.\n"
	"  Core = <core>\n"
	"      CPU core identifier. This is a value between 0 and the number of cores\n"
	"      minus 1, as defined in the CPU configuration file. This variable\n"
	"      should be omitted for GPU entries.\n"
	"  Thread = <thread>\n"
	"      CPU thread identifier. Value between 0 and the number of threads per\n"
	"      core minus 1. Omitted for GPU entries.\n"
	"  ComputeUnit = <id>\n"
	"      GPU compute unit identifier. Value between 0 and the number of compute\n"
	"      units minus 1, as defined in the GPU configuration file. This variable\n"
	"      should be omitted for CPU entries.\n"
	"  DataModule = <mod>\n"
	"  ConstantDataModule = <mod>\n"
	"  InstModule = <mod>\n"
	"      In architectures supporting separate data/instruction caches, modules\n"
	"      used to access memory for each particular purpose.\n"
	"  Module = <mod>\n"
	"      Module used to access the memory hierarchy. For architectures\n"
	"      supporting separate data/instruction caches, this variable can be used\n"
	"      instead of 'DataModule', 'InstModule', and 'ConstantDataModule' to\n"
	"      indicate that data and instruction caches are unified.\n"
	"\n";



/*
 * Private functions
 */

#define MEM_SYSTEM_MAX_LEVELS  10

static char *mem_err_config_note =
	"\tPlease run 'm2s --mem-help' or consult the Multi2Sim Guide for\n"
	"\ta description of the memory system configuration file format.\n";

static char *err_mem_config_net =
	"\tNetwork identifiers need to be declared either in the cache\n"
	"\tconfiguration file, or in the network configuration file (option\n"
	"\t'--net-config').\n";

static char *err_mem_levels =
	"\tThe path from a cache into main memory exceeds 10 levels of cache.\n"
	"\tThis might be a symptom of a recursive reference in 'LowModules'\n"
	"\tlists. If you really intend to have a high number of cache levels,\n"
	"\tincrease variable MEM_SYSTEM_MAX_LEVELS in '" __FILE__ "'\n";

static char *err_mem_block_size =
	"\tBlock size in a cache must be greater or equal than its\n"
	"\tlower-level cache for correct behavior of directories and\n"
	"\tcoherence protocols.\n";

static char *err_mem_connect =
	"\tAn external network is used that does not provide connectivity\n"
	"\tbetween a memory module and an associated low/high module. Please\n"
	"\tadd the necessary links in the network configuration file.\n";

static char *err_mem_disjoint =
	"\tIn current versions of Multi2Sim, it is not allowed having a\n"
	"\tmemory module shared for different architectures. Please make sure\n"
	"\tthat the sets of modules accessible by different architectures\n"
	"\tare disjoint.\n";


char *mod_RTM_type_map[] = {"Nulo","SL","SE","DL"};

static void mem_config_default(struct arch_t *arch, void *user_data)
{
	struct config_t *config = user_data;

	/* Only for architectures in detailed simulation */
	if (arch->sim_kind != arch_sim_kind_detailed)
		return;

	/* Architecture must have registered its 'mem_config_default' function */
	if (!arch->mem_config_default_func)
		panic("%s: no default memory configuration for %s",
				__FUNCTION__, arch->name);

	/* Call function for default memory configuration */
	arch->mem_config_default_func(config);
}


static void mem_config_check(struct arch_t *arch, void *user_data)
{
	struct config_t *config = user_data;

	/* Only for architectures in detailed simulation */
	if (arch->sim_kind != arch_sim_kind_detailed)
		return;

	/* Architecture must have registers 'mem_config_check' function */
	if (!arch->mem_config_check_func)
		panic("%s: not default memory check for %s",
				__FUNCTION__, arch->name);

	/* Call function for memory check */
	arch->mem_config_check_func(config);
}


static void mem_config_read_general(struct config_t *config)
{
	char *section;

	/* Section with general parameters */
	section = "General";

	/* Frequency */
	mem_frequency = config_read_int(config, section,
			"Frequency", mem_frequency);
	if (!IN_RANGE(mem_frequency, 1, ESIM_MAX_FREQUENCY))
		fatal("%s: invalid value for 'Frequency'.\n%s",
			mem_config_file_name, mem_err_config_note);

	/* Page size */
	mmu_page_size = config_read_int(config, section, "PageSize",
			mmu_page_size);
	if ((mmu_page_size & (mmu_page_size - 1)))
		fatal("%s: page size must be power of 2.\n%s",
			mem_config_file_name, mem_err_config_note);

	/* Peer transfers */
	mem_peer_transfers = config_read_bool(config, section,
		"PeerTransfers", 1);
}


static void mem_config_read_networks(struct config_t *config)
{
	struct net_t *net;
	int i;

	char buf[MAX_STRING_SIZE];
	char *section;

	/* Create networks */
	mem_debug("Creating internal networks:\n");
	for (section = config_section_first(config); section;
		section = config_section_next(config))
	{
		char *net_name;

		/* Network section */
		if (strncasecmp(section, "Network ", 8))
			continue;
		net_name = section + 8;

		/* Create network */
		net = net_create(net_name);
		mem_debug("\t%s\n", net_name);
		list_add(mem_system->net_list, net);
	}
	mem_debug("\n");

	/* Add network pointers to configuration file. This needs to be done
	 * separately, because configuration file writes alter enumeration of
	 * sections. Also check integrity of sections. */
	for (i = 0; i < list_count(mem_system->net_list); i++)
	{
		/* Get network section name */
		net = list_get(mem_system->net_list, i);
		snprintf(buf, sizeof buf, "Network %s", net->name);
		assert(config_section_exists(config, buf));

		/* Add pointer */
		config_write_ptr(config, buf, "ptr", net);

		/* Check section integrity */
		config_var_enforce(config, buf, "DefaultInputBufferSize");
		config_var_enforce(config, buf, "DefaultOutputBufferSize");
		config_var_enforce(config, buf, "DefaultBandwidth");
		config_section_check(config, buf);
	}
}


static void mem_config_insert_module_in_network(struct config_t *config,
	struct mod_t *mod, char *net_name, char *net_node_name,
	struct net_t **net_ptr, struct net_node_t **net_node_ptr)
{
	struct net_t *net;
	struct net_node_t *node;

	int def_input_buffer_size;
	int def_output_buffer_size;

	char buf[MAX_STRING_SIZE];

	/* No network specified */
	*net_ptr = NULL;
	*net_node_ptr = NULL;
	if (!*net_name)
		return;

	/* Try to insert in private network */
	snprintf(buf, sizeof buf, "Network %s", net_name);
	net = config_read_ptr(config, buf, "ptr", NULL);
	if (!net)
		goto try_external_network;

	/* For private networks, 'net_node_name' should be empty */
	if (*net_node_name)
		fatal("%s: %s: network node name should be empty.\n%s",
			mem_config_file_name, mod->name,
			mem_err_config_note);

	/* Network should not have this module already */
	if (net_get_node_by_user_data(net, mod))
		fatal("%s: network '%s' already contains module '%s'.\n%s",
			mem_config_file_name, net->name,
			mod->name, mem_err_config_note);

	/* Read buffer sizes from network */
	def_input_buffer_size = config_read_int(config, buf,
		"DefaultInputBufferSize", 0);
	def_output_buffer_size = config_read_int(config, buf,
		"DefaultOutputBufferSize", 0);
	if (!def_input_buffer_size)
	{
		fatal("%s: network %s: variable 'DefaultInputBufferSize' "
			"missing.\n%s", mem_config_file_name, net->name,
			mem_err_config_note);
	}
	if (!def_output_buffer_size)
	{
		fatal("%s: network %s: variable 'DefaultOutputBufferSize' "
			"missing.\n%s", mem_config_file_name, net->name,
			mem_err_config_note);
	}
	if (def_input_buffer_size < mod->block_size + 8)
	{
		fatal("%s: network %s: minimum input buffer size is %d for "
			"cache '%s'.\n%s", mem_config_file_name, net->name,
			mod->block_size + 8, mod->name, mem_err_config_note);
	}
	if (def_output_buffer_size < mod->block_size + 8)
		fatal("%s: network %s: minimum output buffer size is %d for "
			"cache '%s'.\n%s", mem_config_file_name, net->name,
			mod->block_size + 8, mod->name, mem_err_config_note);

	/* Insert module in network */
	node = net_add_end_node(net, def_input_buffer_size,
		def_output_buffer_size, mod->name, mod);

	/* Return */
	*net_ptr = net;
	*net_node_ptr = node;
	return;


try_external_network:

	/* Get network */
	net = net_find(net_name);
	if (!net)
		fatal("%s: %s: invalid network name.\n%s%s",
			mem_config_file_name, net_name,
			mem_err_config_note, err_mem_config_net);

	/* Node name must be specified */
	if (!*net_node_name)
		fatal("%s: %s: network node name required for external "
			"network.\n%s%s", mem_config_file_name, mod->name,
			mem_err_config_note, err_mem_config_net);

	/* Get node */
	node = net_get_node_by_name(net, net_node_name);
	if (!node)
		fatal("%s: network %s: node %s: invalid node name.\n%s%s",
			mem_config_file_name, net_name, net_node_name,
			mem_err_config_note, err_mem_config_net);

	/* No module must have been assigned previously to this node */
	if (node->user_data)
		fatal("%s: network %s: node '%s' already assigned.\n%s",
			mem_config_file_name, net->name,
			net_node_name, mem_err_config_note);

	/* Network should not have this module already */
	if (net_get_node_by_user_data(net, mod))
		fatal("%s: network %s: module '%s' is already present.\n%s",
			mem_config_file_name, net->name,
			mod->name, mem_err_config_note);

	/* Assign module to network node and return */
	node->user_data = mod;
	*net_ptr = net;
	*net_node_ptr = node;
}


static struct prefetcher_t* mem_config_read_prefetcher(struct config_t *config, char *section)
{
	char pref_name[MAX_STRING_SIZE];

	char *type_str;
	char *adapt_policy_str;
	char *adapt_interval_kind_str;

	enum prefetcher_type_t type;
	struct prefetcher_t *pref;

	int ghb_size;
	int it_size;
	int lookup_depth;
	int aggr;

	config_section_enforce(config, section);

	/* Extract pref name */
	str_token(pref_name, sizeof pref_name, section, 0, " ");

	/* General parameters */
	type_str = config_read_string(config, section, "Type", "cz_cs_sb");
	ghb_size = config_read_int(config, section, "GHBSize", 256);
	it_size = config_read_int(config, section, "ITSize", 64);
	lookup_depth = config_read_int(config, section, "LookupDepth", 2);
	aggr = config_read_int(config, section, "Aggressivity", 4); /* If an adaptive policy is used, this is the maximum aggressivity */

	/* Creation */
	type = str_map_string_case_err_msg(&prefetcher_type_map, type_str,
			"%s: prefetcher %s: Invalid prefetcher type", mem_config_file_name, pref_name);
	pref = prefetcher_create(ghb_size, it_size, lookup_depth, type, aggr);

	/* CZone prefetchers */
	pref->czone_bits = config_read_int(config, section, "CZoneBits", 13);
	pref->czone_mask = ~(-1 << pref->czone_bits);

	/* Streaming prefetchers */
	pref->distance = config_read_int(config, section, "Distance", 16); /* Max prefetched blocks at any time in a given stream. If using stream buffers, max depth of the stream buffer. */
	pref->max_num_streams = config_read_int(config, section, "Streams", 4);  /* Number of prefetch streams per cache. If using stream buffers, number of buffers. */
	pref->max_num_slots = config_read_int(config, section, "Slots", pref->distance);
	pref->stream_tag_bits = config_read_int(config, section, "StreamTagBits", sizeof(unsigned int) * 8 - pref->czone_bits);
	pref->stream_tag_mask = -1 << (sizeof(pref->stream_tag_mask) * 8 - pref->stream_tag_bits);
	prefetcher_stream_buffers_create(pref, pref->max_num_streams, pref->max_num_slots);

	/* Adaptive prefetchers */
	pref->aggr_ini = config_read_int(config, section, "InitialAggressivity", aggr);
	adapt_policy_str = config_read_string(config, section, "AdaptPolicy", "none");
	pref->adapt_policy = str_map_string_case_err_msg(&adapt_pref_policy_map, adapt_policy_str,
			"%s: cache %s: Invalid adaptative prefetch policy", mem_config_file_name, pref_name);
	pref->adapt_interval = config_read_llint(config, section, "AdaptInterval", 50000);
	adapt_interval_kind_str = config_read_string(config, section, "AdaptIntervalKind", "cycles");
	pref->adapt_interval_kind = str_map_string_case_err_msg(&interval_kind_map, adapt_interval_kind_str,
			"%s: cache %s: Invalid interval kind", mem_config_file_name, pref_name);

	/* Bloom Filter */
	pref->bloom_bits = config_read_int(config, section, "BloomBits", 0); /* If 0, compute it using the capacity and probability */
	pref->bloom_capacity = config_read_int(config, section, "BloomCapacity", 4096); /* If 0, compute it using the size in bits and probability */
	pref->bloom_false_pos_prob = config_read_double(config, section, "BloomFalsePosProb", 0.05); /* Max false positive probability */

	/* Thresholds */
	prefetcher_set_default_adaptive_thresholds(pref);
	switch (pref->adapt_policy)
	{
		case adapt_pref_policy_adp:
			#define POLICY adp
			#define POLICY_UP "ADP"
			pref->th.POLICY.a1 = config_read_double(config, section, POLICY_UP ".A1", pref->th.POLICY.a1);
			pref->th.POLICY.a2 = config_read_double(config, section, POLICY_UP ".A2", pref->th.POLICY.a2);
			pref->th.POLICY.a3 = config_read_double(config, section, POLICY_UP ".A3", pref->th.POLICY.a3);
			pref->th.POLICY.acc_high = config_read_double(config, section, POLICY_UP ".AccHigh", pref->th.POLICY.acc_high);
			pref->th.POLICY.acc_low = config_read_double(config, section, POLICY_UP ".AccLow", pref->th.POLICY.acc_low);
			pref->th.POLICY.acc_very_low = config_read_double(config, section, POLICY_UP ".AccVeryLow", pref->th.POLICY.acc_very_low);
			pref->th.POLICY.cov = config_read_double(config, section, POLICY_UP ".Cov", pref->th.POLICY.cov);
			pref->th.POLICY.bwno = config_read_double(config, section, POLICY_UP ".BWNO", pref->th.POLICY.bwno);
			pref->th.POLICY.rob_stall = config_read_double(config, section, POLICY_UP ".RobStall", pref->th.POLICY.rob_stall);
			pref->th.POLICY.ipc = config_read_double(config, section, POLICY_UP ".IPC", pref->th.POLICY.ipc);
			pref->th.POLICY.misses = config_read_double(config, section, POLICY_UP ".Misses", pref->th.POLICY.misses);
			#undef POLICY
			#undef POLICY_UP
			break;

		case adapt_pref_policy_hpac:
			#define POLICY hpac
			#define POLICY_UP "HPAC"
			pref->th.POLICY.a1 = config_read_double(config, section, POLICY_UP ".A1", pref->th.POLICY.a1);
			pref->th.POLICY.a2 = config_read_double(config, section, POLICY_UP ".A2", pref->th.POLICY.a2);
			pref->th.POLICY.a3 = config_read_double(config, section, POLICY_UP ".A3", pref->th.POLICY.a3);
			pref->th.POLICY.acc = config_read_double(config, section, POLICY_UP ".Acc", pref->th.POLICY.acc);
			pref->th.POLICY.bwno = config_read_double(config, section, POLICY_UP ".BWNO", pref->th.POLICY.bwno);
			pref->th.POLICY.bwc = config_read_double(config, section, POLICY_UP ".BWC", pref->th.POLICY.bwc);
			pref->th.POLICY.pollution = config_read_double(config, section, POLICY_UP ".Pollution", pref->th.POLICY.pollution);
			#undef POLICY
			#undef POLICY_UP
			/* No 'break' since HPAC needs FDP */

		case adapt_pref_policy_fdp:
			#define POLICY fdp
			#define POLICY_UP "FDP"
			pref->th.POLICY.a1 = config_read_double(config, section, POLICY_UP ".A1", pref->th.POLICY.a1);
			pref->th.POLICY.a2 = config_read_double(config, section, POLICY_UP ".A2", pref->th.POLICY.a2);
			pref->th.POLICY.a3 = config_read_double(config, section, POLICY_UP ".A3", pref->th.POLICY.a3);
			pref->th.POLICY.acc_high = config_read_double(config, section, POLICY_UP ".AccHigh", pref->th.POLICY.acc_high);
			pref->th.POLICY.acc_low = config_read_double(config, section, POLICY_UP ".AccLow", pref->th.POLICY.acc_low);
			pref->th.POLICY.lateness = config_read_double(config, section, POLICY_UP ".Lateness", pref->th.POLICY.lateness);
			pref->th.POLICY.pollution = config_read_double(config, section, POLICY_UP ".Pollution", pref->th.POLICY.pollution);
			#undef POLICY
			#undef POLICY_UP
			break;

		case adapt_pref_policy_mbp:
			#define POLICY mbp
			#define POLICY_UP "MBP"
			pref->th.POLICY.a1 = config_read_double(config, section, POLICY_UP ".A1", pref->th.POLICY.a1);
			pref->th.POLICY.a2 = config_read_double(config, section, POLICY_UP ".A2", pref->th.POLICY.a2);
			pref->th.POLICY.a3 = config_read_double(config, section, POLICY_UP ".A3", pref->th.POLICY.a3);
			pref->th.POLICY.ratio = config_read_double(config, section, POLICY_UP ".Ratio", pref->th.POLICY.ratio);
			#undef POLICY
			#undef POLICY_UP
			break;

		default:
			break;
	}

	/* Check */
	if (ghb_size < 1 || it_size < 1 || lookup_depth < 2 || lookup_depth > PREFETCHER_LOOKUP_DEPTH_MAX)
		fatal("%s: cache %s: invalid prefetcher configuration.\n%s", mem_config_file_name, pref_name, mem_err_config_note);

	if (aggr < 1)
		fatal("%s: cache %s: invalid value for variable 'PrefetcherAggressivity'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->czone_bits < 1 || pref->czone_bits > 32)
		fatal("%s: cache %s: invalid value for variable 'CZoneBits'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->stream_tag_bits < 1 || pref->stream_tag_bits > 32)
		fatal("%s: cache %s: invalid value for variable 'StreamTagBits'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->max_num_streams < 1)
		fatal("%s: cache %s: invalid value for variable 'Streams'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->distance < 1 && pref->distance <= pref->max_num_slots)
		fatal("%s: cache %s: invalid value for variable 'Distance'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->aggr_ini < 1)
		fatal("%s: cache %s: invalid value for variable 'InitialAggressivity'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->bloom_bits < 0)
		fatal("%s: cache %s: invalid value for variable 'BloomBits'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->bloom_capacity < 0)
		fatal("%s: cache %s: invalid value for variable 'BloomCapacity'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	if (pref->bloom_false_pos_prob <= 0 || pref->bloom_false_pos_prob > 1)
		fatal("%s: cache %s: invalid value for variable 'BloomBits'.\n%s",
				mem_config_file_name, pref_name, mem_err_config_note);

	return pref;
}


static struct mod_t *mem_config_read_cache(struct config_t *config,
	char *section)
{
	char buf[MAX_STRING_SIZE];
	char mod_name[MAX_STRING_SIZE];

	int num_sets;
	int assoc;
	int block_size;
	int latency;
	int dir_latency;
	

	char *policy_str;
	enum cache_policy_t policy;

	int mshr_size;
	int num_ports;

	char *net_name;
	char *net_node_name;

	struct mod_t *mod;
	struct net_t *net;
	struct net_node_t *net_node;


	char *prefetcher_str;

	char *partitioning_str;
	enum cache_partitioning_policy_t partitioning_policy = cache_partitioning_policy_none;
	long long partitioning_interval = 5000000;
	enum interval_kind_t partitioning_interval_kind = interval_kind_cycles;

	enum thread_pairing_policy_t pairing_policy = thread_pairing_policy_none;
	long long pairing_interval = 0;

	struct list_t *tokens;


	int i;

	/* Cache parameters */
	snprintf(buf, sizeof buf, "CacheGeometry %s",
		config_read_string(config, section, "Geometry", ""));
	config_var_enforce(config, section, "Geometry");
	config_section_enforce(config, buf);
	config_var_enforce(config, buf, "Latency");
	config_var_enforce(config, buf, "Sets");
	config_var_enforce(config, buf, "Assoc");
	config_var_enforce(config, buf, "BlockSize");

	/* Read values */
	str_token(mod_name, sizeof mod_name, section, 1, " ");
	num_sets = config_read_int(config, buf, "Sets", 16);
	assoc = config_read_int(config, buf, "Assoc", 2);
	block_size = config_read_int(config, buf, "BlockSize", 256);
	latency = config_read_int(config, buf, "Latency", 1);
	dir_latency = config_read_int(config, buf, "DirectoryLatency", 1);
	policy_str = config_read_string(config, buf, "Policy", "LRU");
	mshr_size = config_read_int(config, buf, "MSHR", 16);
	num_ports = config_read_int(config, buf, "Ports", 2);
	/* Cache partitioning */
	partitioning_str = config_read_string(config, buf, "Partitioning", "none");
	tokens = str_token_list_create(partitioning_str, " ");
	LIST_FOR_EACH(tokens, i)
	{
		char *token = list_get(tokens, i);
		int err;

		switch(i)
		{
			/* Cache partitioning */
			case 0:
				partitioning_policy = str_map_string_case_err_msg(&cache_partitioning_policy_map, token,
						"%s: cache %s: Invalid cache partitioning policy", mem_config_file_name, mod_name);
				break;
			case 1:
				partitioning_interval = str_to_llint(token, &err);
				if (err)
					fatal("%s: cache %s: Invalid cache partitioning interval: %s", mem_config_file_name, mod_name, str_error(err));
				break;
			case 2:
				partitioning_interval_kind = str_map_string_case_err_msg(&interval_kind_map, token,
						"%s: cache %s: Invalid partitioning interval kind", mem_config_file_name, mod_name);
				break;
			/* Thread pairing, only for FCP */
			case 3:
				if (partitioning_policy != cache_partitioning_policy_fcp)
					fatal("%s: cache %s: Task pairing is only supported for FCP cache partitioning.\n", mem_config_file_name, mod_name);
				pairing_policy = str_map_string_case_err_msg(&thread_pairing_policy_map, token,
						"%s: cache %s: Invalid task pairing policy", mem_config_file_name, mod_name);
				break;
			case 4:
				pairing_interval = str_to_llint(token, &err);
				if (err)
					fatal("%s: cache %s: Invalid thread pairing interval: %s", mem_config_file_name, mod_name, str_error(err));
				if (pairing_policy == thread_pairing_policy_nearest && pairing_interval != 0)
					fatal("%s: cache %s: Nearest thread pairing policy is static, thread pairs are not modified, therefore the pairing interval must be 0",
							mem_config_file_name, mod_name);
				break;
			default:
				fatal("%s: cache %s: Partitioning policy must be <policy> [<interval> <interval kind>][<pairning> <pair duration in intervals>]."
					"Only first argument is mandatory. Optional arguments indicate when the policy is reevaluated and default to 5M cycles.\n%s", mem_config_file_name, mod_name,
					mem_err_config_note);
				break;
		}
	}
	str_token_list_free(tokens);

	/* Checks */
	policy = str_map_string_case(&cache_policy_map, policy_str);
	if (policy == cache_policy_invalid)
		fatal("%s: cache %s: %s: invalid block replacement policy.\n%s",
			mem_config_file_name, mod_name,
			policy_str, mem_err_config_note);
	if (num_sets < 1 || (num_sets & (num_sets - 1)))
		fatal("%s: cache %s: number of sets must be a power of two "
			"greater than 1.\n%s", mem_config_file_name, mod_name,
			mem_err_config_note);
	if (assoc < 1)
		fatal("%s: cache %s: associativity must be > 1.\n%s", mem_config_file_name, mod_name, mem_err_config_note);
	if (block_size < 4 || (block_size & (block_size - 1)))
		fatal("%s: cache %s: block size must be power of two and "
			"at least 4.\n%s", mem_config_file_name, mod_name,
			mem_err_config_note);
	if (dir_latency < 1)
		fatal("%s: cache %s: invalid value for variable "
			"'DirectoryLatency'.\n%s", mem_config_file_name,
			mod_name, mem_err_config_note);
	if (latency < 1)
		fatal("%s: cache %s: invalid value for variable 'Latency'.\n%s",
			mem_config_file_name, mod_name, mem_err_config_note);
	if (mshr_size < 0)
		fatal("%s: cache %s: invalid value for variable 'MSHR'.\n%s",
			mem_config_file_name, mod_name, mem_err_config_note);
	if (num_ports < 1)
		fatal("%s: cache %s: invalid value for variable 'Ports'.\n%s",
			mem_config_file_name, mod_name, mem_err_config_note);
	if (policy == cache_policy_partitioned_lru && !partitioning_policy)
		warning("%s: cache %s: cache policy is partitioned LRU but no partitioning policy "
				"has been set. It will behave as normal LRU.\n", mem_config_file_name, mod_name);

	/* Create module */
	mod = mod_create(mod_name, mod_kind_cache, num_ports,
		block_size, latency);

	/* Initialize */
	mod->mshr_size = mshr_size;
	mod->dir_assoc = assoc;
	mod->dir_num_sets = num_sets;
	mod->dir_size = num_sets * assoc;
	mod->dir_latency = dir_latency;

	


	/* High network */
	net_name = config_read_string(config, section, "HighNetwork", "");
	net_node_name = config_read_string(config, section,
		"HighNetworkNode", "");
	mem_config_insert_module_in_network(config, mod, net_name, net_node_name,
		&net, &net_node);
	mod->high_net = net;
	mod->high_net_node = net_node;

	/* Low network */
	net_name = config_read_string(config, section, "LowNetwork", "");
	net_node_name = config_read_string(config, section,
		"LowNetworkNode", "");
	mem_config_insert_module_in_network(config, mod, net_name,
		net_node_name, &net, &net_node);
	mod->low_net = net;
	mod->low_net_node = net_node;

	/* Create cache */
	mod->cache = cache_create(mod->name, num_sets, block_size, assoc, policy);

	/* Create prefetcher */
	prefetcher_str = config_read_string(config, buf, "Prefetcher", "");
	if (strlen(prefetcher_str))
	{
		snprintf(buf, sizeof buf, " Prefetcher %s ", prefetcher_str);
		mod->cache->prefetcher = mem_config_read_prefetcher(config, buf);
		mod->cache->prefetcher->parent_cache = mod->cache;
	}

	/* Partitioning policy */
	mod->cache->partitioning.policy = partitioning_policy;
	mod->cache->partitioning.interval = partitioning_interval;
	mod->cache->partitioning.interval_kind = partitioning_interval_kind;

	/* Pairing policy */
	mod->cache->pairing.policy = pairing_policy;
	mod->cache->pairing.interval = pairing_interval;
	

	/* Return */
	return mod;
}


static struct mod_t *mem_config_read_main_memory(struct config_t *config,
	char *section)
{
	char *dram_system_name;
	char *net_name;
	char *net_node_name;

	char mod_name[MAX_STRING_SIZE];

	int block_size; /* Broken */
	int latency; /* Only if not using dramsim */
	int num_ports;
	int dir_size;
	int dir_assoc;

	struct mod_t *mod;
	struct net_t *net;
	struct net_node_t *net_node;

	
       
        /* Read parameters */
	str_token(mod_name, sizeof mod_name, section, 1, " ");
	config_var_enforce(config, section, "Latency");
	config_var_enforce(config, section, "BlockSize");
	block_size = config_read_int(config, section, "BlockSize", 64);
	latency = config_read_int(config, section, "Latency", 1);
	num_ports = config_read_int(config, section, "Ports", 2);
	dir_size = config_read_int(config, section, "DirectorySize", 1024);
	dir_assoc = config_read_int(config, section, "DirectoryAssoc", 8);
	dram_system_name = config_read_string(config, section, "DRAMSystem", "");


	/* Check parameters */
	if (block_size < 1 || (block_size & (block_size - 1)))
		fatal("%s: %s: block size must be power of two.\n%s",
				mem_config_file_name, mod_name, mem_err_config_note);
	if (latency < 1)
		fatal("%s: %s: invalid value for variable 'Latency'.\n%s",
				mem_config_file_name, mod_name, mem_err_config_note);
	if (num_ports < 1)
		fatal("%s: %s: invalid value for variable 'NumPorts'.\n%s",
				mem_config_file_name, mod_name, mem_err_config_note);
	if (dir_size < 1 || (dir_size & (dir_size - 1)))
		fatal("%s: %s: directory size must be a power of two.\n%s",
				mem_config_file_name, mod_name, mem_err_config_note);
	if (dir_assoc < 1 || (dir_assoc & (dir_assoc - 1)))
		fatal("%s: %s: directory associativity must be a power of "
				"two.\n%s", mem_config_file_name, mod_name, mem_err_config_note);
	if (dir_assoc > dir_size)
		fatal("%s: %s: invalid directory associativity.\n%s",
				mem_config_file_name, mod_name, mem_err_config_note);

	/* Create module */
	mod = mod_create(mod_name, mod_kind_main_memory, num_ports,
			block_size, latency);

	/* Store directory size */
	mod->dir_size = dir_size;
	mod->dir_assoc = dir_assoc;
	mod->dir_num_sets = dir_size / dir_assoc;

	/* High network */
	net_name = config_read_string(config, section, "HighNetwork", "");
	net_node_name = config_read_string(config, section, "HighNetworkNode", "");
	mem_config_insert_module_in_network(config, mod, net_name,
			net_node_name, &net, &net_node);
	mod->high_net = net;
	mod->high_net_node = net_node;

	/* Create cache and directory */
	mod->cache = cache_create(mod->name, dir_size / dir_assoc, block_size,
			dir_assoc, cache_policy_lru);

	/* Connect to specified main mem system, if any */
	mod->dram_system = hash_table_get(mem_system->dram_systems, dram_system_name);
	if (mod->dram_system)
		mod->mc_id = mod->dram_system->num_mcs++; /* Asign an ID and increment the number of memory controllers in the dram system */

	list_add(mem_system->mm_mod_list, mod);

	
	/*Return */
	return mod;
}


static void mem_config_read_module_address_range(struct config_t *config,
	struct mod_t *mod, char *section)
{
	char *range_str;
	char *token;
	char *delim;

	int err;

	/* Read address range */
	range_str = config_read_string(config, section, "AddressRange", "");
	if (!*range_str)
	{
		mod->range_kind = mod_range_bounds;
		mod->range.bounds.low = 0;
		mod->range.bounds.high = -1;
		return;
	}

	/* Split in tokens */
	range_str = xstrdup(range_str);
	delim = " ";
	token = strtok(range_str, delim);
	if (!token)
		goto invalid_format;

	/* First token - ADDR or BOUNDS */
	if (!strcasecmp(token, "BOUNDS"))
	{
		/* Format is: BOUNDS <low> <high> */
		mod->range_kind = mod_range_bounds;

		/* Low bound */
		if (!(token = strtok(NULL, delim)))
			goto invalid_format;
		mod->range.bounds.low = str_to_int(token, &err);
		if (err)
			fatal("%s: %s: invalid value '%s' in 'AddressRange'",
				mem_config_file_name, mod->name, token);
		if (mod->range.bounds.low % mod->block_size)
			fatal("%s: %s: low address bound must be a multiple "
				"of block size.\n%s", mem_config_file_name,
				mod->name, mem_err_config_note);

		/* High bound */
		if (!(token = strtok(NULL, delim)))
			goto invalid_format;
		mod->range.bounds.high = str_to_int(token, &err);
		if (err)
			fatal("%s: %s: invalid value '%s' in 'AddressRange'",
				mem_config_file_name, mod->name, token);
		if ((mod->range.bounds.high + 1) % mod->block_size)
			fatal("%s: %s: high address bound must be a multiple "
				"of block size minus 1.\n%s",
				mem_config_file_name, mod->name,
				mem_err_config_note);

		/* No more tokens */
		if ((token = strtok(NULL, delim)))
			goto invalid_format;
	}
	else if (!strcasecmp(token, "ADDR"))
	{
		/* Format is: ADDR DIV <div> MOD <mod> EQ <eq> */
		mod->range_kind = mod_range_interleaved;

		/* Token 'DIV' */
		if (!(token = strtok(NULL, delim)) || strcasecmp(token, "DIV"))
			goto invalid_format;

		/* Field <div> */
		if (!(token = strtok(NULL, delim)))
			goto invalid_format;
		mod->range.interleaved.div = str_to_int(token, &err);
		if (err)
			fatal("%s: %s: invalid value '%s' in 'AddressRange'",
				mem_config_file_name, mod->name, token);
		if (mod->range.interleaved.div < 1)
			goto invalid_format;
		if (mod->range.interleaved.div % mod->block_size)
			fatal("%s: %s: value for <div> must be a multiple of block size.\n%s",
				mem_config_file_name, mod->name, mem_err_config_note);

		/* Token 'MOD' */
		if (!(token = strtok(NULL, delim)) || strcasecmp(token, "MOD"))
			goto invalid_format;

		/* Field <mod> */
		if (!(token = strtok(NULL, delim)))
			goto invalid_format;
		mod->range.interleaved.mod = str_to_int(token, &err);
		if (err)
			fatal("%s: %s: invalid value '%s' in 'AddressRange'",
				mem_config_file_name, mod->name, token);
		if (mod->range.interleaved.mod < 1)
			goto invalid_format;

		/* Token 'EQ' */
		if (!(token = strtok(NULL, delim)) || strcasecmp(token, "EQ"))
			goto invalid_format;

		/* Field <eq> */
		if (!(token = strtok(NULL, delim)))
			goto invalid_format;
		mod->range.interleaved.eq = str_to_int(token, &err);
		if (err)
			fatal("%s: %s: invalid value '%s' in 'AddressRange'",
				mem_config_file_name, mod->name, token);
		if (mod->range.interleaved.eq >= mod->range.interleaved.mod)
			goto invalid_format;

		/* No more tokens */
		if ((token = strtok(NULL, delim)))
			goto invalid_format;
	}
	else
	{
		goto invalid_format;
	}

	/* Free string duplicate */
	free(range_str);
	return;

invalid_format:

	fatal("%s: %s: invalid format for 'AddressRange'.\n%s",
		mem_config_file_name, mod->name, mem_err_config_note);
}


static void mem_config_read_dram_systems(struct config_t *config)
{
	struct dram_system_handler_t *handler;
	struct dram_system_t *dram_system;

	char *section;
	char *device_config_str; /* Path to INI file */
	char *system_config_str; /* Path to INI file */
	char *report_file_str;

	char dram_system_name[MAX_STRING_SIZE];
	char dram_system_intrep_file[MAX_PATH_SIZE];

	double dram_system_freq;

	int megabytes; /* Total size */
	int ret;

	/* Create main memory systems */
	mem_debug("Creating main memory systems:\n");
	for (section = config_section_first(config); section;
		section = config_section_next(config))
	{
		/* Section for a main memory system */
		if (strncasecmp(section, "DRAMSystem ", 11))
			continue;

		/* Read config parameters */
		str_token(dram_system_name, sizeof dram_system_name, section, 1, " ");
		device_config_str = config_read_string(config, section, "DeviceDescription", "ini/DDR2_micron_16M_8b_x8_sg3E.ini");
		system_config_str = config_read_string(config, section, "SystemDescription", "system.ini");

		ret = snprintf(dram_system_intrep_file, MAX_PATH_SIZE, "%s/%s.csv", dram_interval_reports_dir, dram_system_name);
		if (ret < 0 || ret >= MAX_PATH_SIZE)
			fatal("%s: string too long", dram_system_intrep_file);

		report_file_str = config_read_string(config, section, "ReportFile", dram_system_intrep_file);
		megabytes = config_read_int(config, section, "MB", 4096);
			
		/* Create a handler to the underlying dramsim c++ objects */
		handler = dram_system_create(device_config_str, system_config_str, megabytes, report_file_str);

		/* Create a wrapper to store multi2sim related data and dramsim handler */
		dram_system = xcalloc(1, sizeof(struct dram_system_t));
		dram_system->name = xstrdup(dram_system_name);
		dram_system->handler = handler;
		dram_system->pending_reads = linked_list_create();

		/* Configure dramsim using the handler */
		dram_system_set_cpu_freq(handler, (long long) arch_x86->frequency * 1000000); /* Freq must be in Hz */
		dram_system_freq = dram_system_get_dram_freq(handler) / 1000000; /* Convert freq. to MHz */
		assert(dram_system_freq);

		dram_system_set_epoch_length(handler, epoch_length * (dram_system_freq / esim_frequency)); /* Epoch length for dram in dram cycles */
		dram_system_register_payloaded_callbacks(handler, dram_system, main_memory_read_callback, main_memory_write_callback, main_memory_power_callback);

		/* Add dram system to hash table */
		hash_table_insert(mem_system->dram_systems, dram_system_name, dram_system);
		mem_debug("\t%s\n", dram_system_name);

		/* Schedule an event to notify dramsim that a cycle has passed */
		main_memory_tic_scheduler(dram_system);
	}

	/* Debug */
	mem_debug("\n");
}


static void mem_config_read_modules(struct config_t *config)
{
	struct mod_t *mod;

	char *section;
	char *mod_type;
	char *prefetcher_str;

	char buf[MAX_STRING_SIZE];
	char mod_name[MAX_STRING_SIZE];
        
	//Hugo adding penalty by head movement
        //int mov_cabezal;
        int RTM;
	int i;


	/* Create modules */
	mem_debug("Creating modules:\n");
	for (section = config_section_first(config); section;
		section = config_section_next(config))
	{
		/* Section for a module */
		if (strncasecmp(section, "Module ", 7))
			continue;
		/* Create module, depending on the type. */
		str_token(mod_name, sizeof mod_name, section, 1, " ");

		mod_type = config_read_string(config, section, "Type", "");

		if (!strcasecmp(mod_type, "Cache"))
		{
			mod = mem_config_read_cache(config, section);

			/* Create prefetcher, replacing any prefetcher set in cache architecture */
			prefetcher_str = config_read_string(config, section, "Prefetcher", "");
			if (strlen(prefetcher_str))
			{
				prefetcher_free(mod->cache->prefetcher);
				snprintf(buf, sizeof buf, " Prefetcher %s ", prefetcher_str);
				mod->cache->prefetcher = mem_config_read_prefetcher(config, buf);
				mod->cache->prefetcher->parent_cache = mod->cache;
			}
		}
		else if (!strcasecmp(mod_type, "MainMemory"))
			mod = mem_config_read_main_memory(config, section);
		else
			fatal("%s: %s: invalid or missing value for 'Type'.\n%s",
					mem_config_file_name, mod_name,
					mem_err_config_note);
		//Hugo reading  new vars in the config
       		//mov_cabezal = config_read_int(config, section, "PenalizacionCabezal", 0);
       		RTM = 0;
        	RTM = config_read_int(config, section, "RTM", 0);
		

        	/*Penalty by moving the headers, Hugo */
        	//mod->cache->mov_cabezal = mov_cabezal;

        	/*RTM variables */		
        	mod->RTM = RTM;

		mod->RTM_type = config_read_enum(config, section, "RTM_Type", 0, mod_RTM_type_map, 4); 
		mod->headers = config_read_int(config, section, "headers", 0);
		mod->entrelazado = config_read_int(config, section, "entrelazado", 0); 
		mod->WU = config_read_int(config, section, "WU", 0);
		mod->WU_f = 0;
		mod->submodulos = config_read_int(config, section, "submodulos", 1);
		mod->custom_read_latency =  config_read_int(config, section, "L1D_DWM_read_latency", mod->dir_latency);
		mod->custom_write_latency =  config_read_int(config, section, "L1D_DWM_write_latency", mod->dir_latency);
		mod->TapeCache = config_read_int(config, section, "TapeCache", 0);
		 

		if (mod->RTM || mod->TapeCache)
                {
                        mod->RTM_data = RTM_data_create(mod->cache->num_sets, mod->cache->assoc, mod->headers,mod->submodulos);
			mod_RTM = mod;
                }
		//End


		/* Read module address range */
		mem_config_read_module_address_range(config, mod, section);

		/* Add module */
		list_add(mem_system->mod_list, mod);
		mem_debug("\t%s\n", mod_name);
	}

	/* Debug */
	mem_debug("\n");

	/* Add module pointers to configuration file. This needs to be done
	 * separately, because configuration file writes alter enumeration of
	 * sections.  Also check integrity of sections. */
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		/* Get module and section */
		mod = list_get(mem_system->mod_list, i);
		snprintf(buf, sizeof buf, "Module %s", mod->name);
		assert(config_section_exists(config, buf));
		config_write_ptr(config, buf, "ptr", mod);
	}
}


static void mem_config_check_route_to_main_memory(struct mod_t *mod,
	int block_size, int level)
{
	struct mod_t *low_mod;
	int i;

	/* Maximum level */
	if (level > MEM_SYSTEM_MAX_LEVELS)
		fatal("%s: %s: too many cache levels.\n%s%s",
			mem_config_file_name, mod->name,
			err_mem_levels, mem_err_config_note);

	/* Check block size */
	if (mod->block_size < block_size)
		fatal("%s: %s: decreasing block size.\n%s%s",
			mem_config_file_name, mod->name,
			err_mem_block_size, mem_err_config_note);
	block_size = mod->block_size;

	/* Dump current module */
	mem_debug("\t");
	for (i = 0; i < level * 2; i++)
		mem_debug(" ");
	mem_debug("%s\n", mod->name);

	/* Check that cache has a way to main memory */
	if (!linked_list_count(mod->low_mod_list) && mod->kind == mod_kind_cache)
		fatal("%s: %s: main memory not accessible from cache.\n%s",
			mem_config_file_name, mod->name,
			mem_err_config_note);

	/* Dump children */
	for (linked_list_head(mod->low_mod_list); !linked_list_is_end(mod->low_mod_list);
		linked_list_next(mod->low_mod_list))
	{
		low_mod = linked_list_get(mod->low_mod_list);
		mem_config_check_route_to_main_memory(low_mod, block_size, level + 1);
	}
}


static void mem_config_read_low_modules(struct config_t *config)
{
	char buf[MAX_STRING_SIZE];
	char *delim;

	char *low_mod_name;
	char *low_mod_name_list;

	struct mod_t *mod;
	struct mod_t *low_mod;

	int i;

	/* Lower level modules */
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		/* Get cache module */
		mod = list_get(mem_system->mod_list, i);
		if (mod->kind != mod_kind_cache)
			continue;

		/* Section name */
		snprintf(buf, sizeof buf, "Module %s", mod->name);
		assert(config_section_exists(config, buf));

		/* Low module name list */
		low_mod_name_list = config_read_string(config, buf, "LowModules", "");
		if (!*low_mod_name_list)
			fatal("%s: [ %s ]: missing or invalid value for 'LowModules'.\n%s",
				mem_config_file_name, buf, mem_err_config_note);

		/* For each element in the list */
		low_mod_name_list = xstrdup(low_mod_name_list);
		delim = ", ";
		for (low_mod_name = strtok(low_mod_name_list, delim);
			low_mod_name; low_mod_name = strtok(NULL, delim))
		{
			/* Check valid module name */
			snprintf(buf, sizeof buf, "Module %s", low_mod_name);
			if (!config_section_exists(config, buf))
				fatal("%s: %s: invalid module name in 'LowModules'.\n%s",
					mem_config_file_name, mod->name,
					mem_err_config_note);

			/* Get low cache and assign */
			low_mod = config_read_ptr(config, buf, "ptr", NULL);
			assert(low_mod);
			linked_list_add(mod->low_mod_list, low_mod);
			linked_list_add(low_mod->high_mod_list, mod);
		}

		/* Free copy of low module name list */
		free(low_mod_name_list);
	}

	/* Check paths to main memory */
	mem_debug("Checking paths between caches and main memories:\n");
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		mod = list_get(mem_system->mod_list, i);
		mem_config_check_route_to_main_memory(mod, mod->block_size, 1);
	}
	mem_debug("\n");
}


static void mem_config_read_entries(struct config_t *config)
{
	char *section;
	char *entry_name;
	char *arch_name;

	char entry_name_trimmed[MAX_STRING_SIZE];
	char arch_name_trimmed[MAX_STRING_SIZE];
	char arch_list_names[MAX_STRING_SIZE];

	struct arch_t *arch;

	/* Debug */
	mem_debug("Processing entries to the memory system:\n");
	mem_debug("\n");

	/* Read all [Entry <name>] sections */
	for (section = config_section_first(config); section; section = config_section_next(config))
	{
		/* Discard if not an entry section */
		if (strncasecmp(section, "Entry ", 6))
			continue;

		/* Name for the entry */
		entry_name = section + 6;
		str_trim(entry_name_trimmed, sizeof entry_name_trimmed, entry_name);
		if (!entry_name_trimmed[0])
			fatal("%s: section [%s]: invalid entry name.\n%s",
				mem_config_file_name, section, mem_err_config_note);

		/* Check if variable 'Type' is used in the section. This variable was used in
		 * previous versions, now it is replaced with 'Arch'. */
		if (config_var_exists(config, section, "Type"))
			fatal("%s: section [%s]: Variable 'Type' is obsolete, use 'Arch' instead.\n%s",
				mem_config_file_name, section, mem_err_config_note);

		/* Read architecture in variable 'Arch' */
		arch_name = config_read_string(config, section, "Arch", NULL);
		if (!arch_name)
			fatal("%s: section [%s]: Variable 'Arch' is missing.\n%s",
				mem_config_file_name, section, mem_err_config_note);

		/* Get architecture */
		str_trim(arch_name_trimmed, sizeof arch_name_trimmed, arch_name);
		arch = arch_get(arch_name_trimmed);
		if (!arch)
		{
			arch_get_names(arch_list_names, sizeof arch_list_names);
			fatal("%s: section [%s]: '%s' is an invalid value for 'Arch'.\n"
				"\tPossible values are %s.\n%s",
				mem_config_file_name, section, arch_name_trimmed,
				arch_list_names, mem_err_config_note);
		}

		/* An architecture with an entry in the memory configuration file must
		 * undergo a detailed simulation. */
		if (arch->sim_kind == arch_sim_kind_functional)
			fatal("%s: section [%s]: %s architecture not under detailed simulation.\n"
				"\tA CPU/GPU architecture uses functional simulation by default. Please\n"
				"\tactivate detailed simulation for the %s architecture using command-line\n"
				"\toption '--%s-sim detailed' to use this memory entry.\n",
				mem_config_file_name, section, arch->name, arch->name, arch->prefix);

		/* Check that callback functions are valid */
		if (!arch->mem_config_parse_entry_func)
			fatal("%s: section [%s]: %s architecture does not support entries.\n"
				"\tPlease contact development@multi2sim.org to report this problem.\n",
				mem_config_file_name, section, arch->name);

		/* Call function to process entry. Each architecture implements its own ways
		 * to process entries to the memory hierarchy. */
		arch->mem_config_parse_entry_func(config, section);
	}

	/* After processing all [Entry <name>] sections, check that all architectures
	 * satisfy their entries to the memory hierarchy. */
	arch_for_each(mem_config_check, config);
}


static void mem_config_create_switches(struct config_t *config)
{
	struct net_t *net;
	struct net_node_t *net_node;
	struct net_node_t *net_switch;

	char buf[MAX_STRING_SIZE];
	char net_switch_name[MAX_STRING_SIZE];

	int def_bandwidth;
	int def_input_buffer_size;
	int def_output_buffer_size;

	int i;
	int j;

	/* For each network, add a switch and create node connections */
	mem_debug("Creating network switches and links for internal networks:\n");
	for (i = 0; i < list_count(mem_system->net_list); i++)
	{
		/* Get network and lower level cache */
		net = list_get(mem_system->net_list, i);

		/* Get switch bandwidth */
		snprintf(buf, sizeof buf, "Network %s", net->name);
		assert(config_section_exists(config, buf));
		def_bandwidth = config_read_int(config, buf, "DefaultBandwidth", 0);
		if (def_bandwidth < 1)
			fatal("%s: %s: invalid or missing value for 'DefaultBandwidth'.\n%s",
				mem_config_file_name, net->name, mem_err_config_note);

		/* Get input/output buffer sizes.
		 * Checks for these variables has done before. */
		def_input_buffer_size = config_read_int(config, buf, "DefaultInputBufferSize", 0);
		def_output_buffer_size = config_read_int(config, buf, "DefaultOutputBufferSize", 0);
		assert(def_input_buffer_size > 0);
		assert(def_output_buffer_size > 0);

		/* Create switch */
		snprintf(net_switch_name, sizeof net_switch_name, "Switch");
		net_switch = net_add_switch(net, def_input_buffer_size, def_output_buffer_size,
			def_bandwidth, net_switch_name);
		mem_debug("\t%s.Switch ->", net->name);

		/* Create connections between switch and every end node */
		for (j = 0; j < list_count(net->node_list); j++)
		{
			net_node = list_get(net->node_list, j);
			if (net_node != net_switch)
			{
				net_add_bidirectional_link(net, net_node, net_switch,
					def_bandwidth, def_output_buffer_size,def_input_buffer_size, 1);
				mem_debug(" %s", net_node->name);
			}
		}

		/* Calculate routes */
		net_routing_table_initiate(net->routing_table);
		net_routing_table_floyd_warshall(net->routing_table);

		/* Debug */
		mem_debug("\n");
	}
	mem_debug("\n");

}


static void mem_config_check_routes(void)
{
	struct mod_t *mod;
	struct net_routing_table_entry_t *entry;
	int i;

	/* For each module, check accessibility to low/high modules */
	mem_debug("Checking accessibility to low and high modules:\n");
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		struct mod_t *low_mod;

		/* Get module */
		mod = list_get(mem_system->mod_list, i);
		mem_debug("\t%s\n", mod->name);

		/* List of low modules */
		mem_debug("\t\tLow modules:");
		for (linked_list_head(mod->low_mod_list); !linked_list_is_end(mod->low_mod_list);
			linked_list_next(mod->low_mod_list))
		{
			/* Get low module */
			low_mod = linked_list_get(mod->low_mod_list);
			mem_debug(" %s", low_mod->name);

			/* Check that nodes are in the same network */
			if (mod->low_net != low_mod->high_net)
				fatal("%s: %s: low node '%s' is not in the same network.\n%s",
					mem_config_file_name, mod->name, low_mod->name,
					mem_err_config_note);

			/* Check that there is a route */
			entry = net_routing_table_lookup(mod->low_net->routing_table,
				mod->low_net_node, low_mod->high_net_node);

			if (!entry->output_buffer)
				fatal("%s: %s: network does not connect '%s' with '%s'.\n%s",
					mem_config_file_name, mod->low_net->name,
					mod->name, low_mod->name, err_mem_connect);
		}

		/* List of high modules */
		mem_debug("\n\t\tHigh modules:");
		for (linked_list_head(mod->high_mod_list); !linked_list_is_end(mod->high_mod_list);
			linked_list_next(mod->high_mod_list))
		{
			struct mod_t *high_mod;

			/* Get high module */
			high_mod = linked_list_get(mod->high_mod_list);
			mem_debug(" %s", high_mod->name);

			/* Check that nodes are in the same network */
			if (mod->high_net != high_mod->low_net)
				fatal("%s: %s: high node '%s' is not in the same network.\n%s",
					mem_config_file_name, mod->name, high_mod->name,
					mem_err_config_note);

			/* Check that there is a route */
			entry = net_routing_table_lookup(mod->high_net->routing_table,
				mod->high_net_node, high_mod->low_net_node);
			if (!entry->output_buffer)
				fatal("%s: %s: network does not connect '%s' with '%s'.\n%s",
					mem_config_file_name, mod->high_net->name,
					mod->name, high_mod->name, err_mem_connect);
		}

		/* Debug */
		mem_debug("\n");
	}

	/* Debug */
	mem_debug("\n");
}


/* Recursive test-and-set of module architecture. If module 'mod' or any of its lower-level
 * modules is set to an architecture other than 'arch', return this other architecture.
 * Otherwise, set the architecture of 'mod' and all its lower-level modules to 'arch', and
 * return 'arch'. */
static struct arch_t *mem_config_set_mod_arch(struct mod_t *mod, struct arch_t *arch)
{
	struct mod_t *low_mod;
	struct arch_t *low_mod_arch;

	/* This module has the color */
	if (mod->arch)
		return mod->arch;

	/* Check lower-level modules */
	LINKED_LIST_FOR_EACH(mod->low_mod_list)
	{
		low_mod = linked_list_get(mod->low_mod_list);
		low_mod_arch = mem_config_set_mod_arch(low_mod, arch);
		if (low_mod_arch != arch)
			return low_mod_arch;
	}

	/* Architecture was not set. Set it and return it. */
	mod->arch = arch;
	return arch;
}


static void mem_config_check_disjoint(struct arch_t *arch, void *user_data)
{
	struct arch_t *mod_arch;
	struct mod_t *mod;

	/* Color modules for this architecture */
	LINKED_LIST_FOR_EACH(arch->mem_entry_mod_list)
	{
		mod = linked_list_get(arch->mem_entry_mod_list);
		mod_arch = mem_config_set_mod_arch(mod, arch);
		if (mod_arch != arch)
			fatal("%s: architectures '%s' and '%s' share memory modules.\n%s",
				mem_config_file_name, arch->name, mod_arch->name, err_mem_disjoint);
	}
}


static void mem_config_calculate_sub_block_sizes(void)
{
	struct mod_t *mod;
	struct mod_t *high_mod;
	struct prefetcher_t *pref;

	int num_nodes;
	int i;

	mem_debug("Creating directories:\n");
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		/* Get module */
		mod = list_get(mem_system->mod_list, i);
		pref = mod->cache->prefetcher;

		/* Calculate sub-block size */
		mod->sub_block_size = mod->block_size;
		for (linked_list_head(mod->high_mod_list); !linked_list_is_end(mod->high_mod_list);
			linked_list_next(mod->high_mod_list))
		{
			high_mod = linked_list_get(mod->high_mod_list);
			mod->sub_block_size = MIN(mod->sub_block_size, high_mod->block_size);
		}

		/* Get number of nodes for directory */
		if (mod->high_net && list_count(mod->high_net->node_list))
			num_nodes = list_count(mod->high_net->node_list);
		else
			num_nodes = 1;

		/* Create directory */
		mod->num_sub_blocks = mod->block_size / mod->sub_block_size;
		mod->dir = dir_create(mod->name, mod->dir_num_sets, mod->dir_assoc, mod->num_sub_blocks, num_nodes);
		if (prefetcher_uses_stream_buffers(pref))
			dir_stream_buffers_create(mod->dir, pref->max_num_streams, pref->max_num_slots);
		mem_debug("\t%s - %dx%dx%d (%dx%dx%d effective) - %d entries, %d sub-blocks\n",
			mod->name, mod->dir_num_sets, mod->dir_assoc, num_nodes,
			mod->dir_num_sets, mod->dir_assoc, linked_list_count(mod->high_mod_list),
			mod->dir_size, mod->num_sub_blocks);
	}

	/* Debug */
	mem_debug("\n");
}


static void mem_config_set_mod_level(struct mod_t *mod, int level)
{
	struct mod_t *low_mod;

	/* If level is already set, do nothing */
	if (mod->level >= level)
		return;

	/* Set max level */
	if (level > max_mod_level) max_mod_level = level;

	/* Set level of module and lower modules */
	mod->level = level;
	LINKED_LIST_FOR_EACH(mod->low_mod_list)
	{
		low_mod = linked_list_get(mod->low_mod_list);
		mem_config_set_mod_level(low_mod, level + 1);
	}
}


static void mem_config_calculate_mod_levels_arch(struct arch_t *arch,
		void *user_data)
{
	struct mod_t *mod;

	LINKED_LIST_FOR_EACH(arch->mem_entry_mod_list)
	{
		mod = linked_list_get(arch->mem_entry_mod_list);
		mem_config_set_mod_level(mod, 1);
	}
}


static void mem_config_calculate_mod_levels(void)
{
	struct mod_t *mod;
	int i;

	/* Start recursive level assignment with L1 modules (entries to memory)
	 * for all architectures. */
	arch_for_each(mem_config_calculate_mod_levels_arch, NULL);

	/* Debug */
	mem_debug("Calculating module levels:\n");
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		mod = list_get(mem_system->mod_list, i);
		mem_debug("\t%s -> ", mod->name);
		if (mod->level)
			mem_debug("level %d\n", mod->level);
		else
			mem_debug("not accessible\n");
	}
	mem_debug("\n");
}


/* Version of memory system trace producer.
 * See 'src/visual/memory/mem-system.c' for the trace consumer. */

#define MEM_SYSTEM_TRACE_VERSION_MAJOR		1
#define MEM_SYSTEM_TRACE_VERSION_MINOR		678

static void mem_config_trace(void)
{
	struct net_t *net;
	int i;

	/* No need if not tracing */
	if (!mem_tracing())
		return;

	/* Initialization */
	mem_trace_header("mem.init version=\"%d.%d\"\n",
		MEM_SYSTEM_TRACE_VERSION_MAJOR, MEM_SYSTEM_TRACE_VERSION_MINOR);

	/* Internal networks */
	LIST_FOR_EACH(mem_system->net_list, i)
	{
		net = list_get(mem_system->net_list, i);
		mem_trace_header("mem.new_net name=\"%s\" num_nodes=%d\n",
			net->name, net->node_list->count);
	}

	/* External networks */
	for (net = net_find_first(); net; net = net_find_next())
	{
		mem_trace_header("mem.new_net name=\"%s\" num_nodes=%d\n",
			net->name, net->node_list->count);
	}

	/* Modules */
	LIST_FOR_EACH(mem_system->mod_list, i)
	{
		struct mod_t *mod;

		char *high_net_name;
		char *low_net_name;

		int high_net_node_index;
		int low_net_node_index;

		/* Get module */
		mod = list_get(mem_system->mod_list, i);

		/* If module is unreachable, ignore it */
		if (!mod->level)
			continue;

		/* High network */
		high_net_name = mod->high_net ? mod->high_net->name : "";
		high_net_node_index = mod->high_net_node ? mod->high_net_node->index : 0;

		/* Low network */
		low_net_name = mod->low_net ? mod->low_net->name : "";
		low_net_node_index = mod->low_net_node ? mod->low_net_node->index : 0;

		/* Trace header */
		mem_trace_header("mem.new_mod name=\"%s\" num_sets=%d assoc=%d "
			"block_size=%d sub_block_size=%d num_sharers=%d level=%d "
			"high_net=\"%s\" high_net_node=%d low_net=\"%s\" low_net_node=%d\n",
			mod->name, mod->cache->num_sets, mod->cache->assoc,
			mod->cache->block_size, mod->sub_block_size, mod->dir->num_nodes,
			mod->level, high_net_name, high_net_node_index,
			low_net_name, low_net_node_index);
	}
}


static void mem_config_read_commands(struct config_t *config)
{
	char *section = "Commands";
	char *command_line;
	char command_var[MAX_STRING_SIZE];

	int command_var_id;

	/* Check if section is present */
	if (!config_section_exists(config, section))
		return;

	/* Read commands */
	command_var_id = 0;
	while (1)
	{
		/* Get command */
		snprintf(command_var, sizeof command_var, "Command[%d]", command_var_id);
		command_line = config_read_string(config, section, command_var, NULL);
		if (!command_line)
			break;

		/* Schedule event to process command */
		command_line = xstrdup(command_line);
		esim_schedule_event(EV_MEM_SYSTEM_COMMAND, command_line, 0);

		/* Next command */
		command_var_id++;
	}
}


/* Set in each module the main memory modules it can access */
static void mem_config_main_memory_reachability()
{
	int i;
	struct list_t *stack = list_create();

	/* Complete dram_system reachability for main memory modules */
	LIST_FOR_EACH(mem_system->mm_mod_list, i)
	{
		struct mod_t *mod = list_get(mem_system->mm_mod_list, i);
		assert(mod->kind == mod_kind_main_memory);
		assert(!list_count(mod->reachable_mm_modules));
		list_add(mod->reachable_mm_modules, mod);
		list_push(stack, mod);
	}

	/* Process pending modules */
	while (list_count(stack))
	{
		struct mod_t *mod = list_pop(stack);
		assert(list_count(mod->reachable_mm_modules));
		LINKED_LIST_FOR_EACH(mod->high_mod_list)
		{
			struct mod_t *high_mod = linked_list_get(mod->high_mod_list);
			LIST_FOR_EACH(mod->reachable_mm_modules, i)
			{
				struct mod_t *mm_mod = list_get(mod->reachable_mm_modules, i);
				if (list_index_of(high_mod->reachable_mm_modules, mm_mod) == -1) /* If not found */
					list_add(high_mod->reachable_mm_modules, mm_mod);
			}
			list_push(stack, high_mod);
		}
	}
	list_free(stack);
}


/* Set in each module the threads that can access it */
static void mem_config_x86_thread_reachability()
{
	struct list_t *stack = list_create();
	int total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;
	int m;

	if (arch_x86->sim_kind != arch_sim_kind_detailed)
		return;

	for (int core = 0; core < x86_cpu_num_cores; core++)
	{
		for (int thread = 0; thread < x86_cpu_num_threads; thread++)
		{
			X86_THREAD.data_mod->reachable_threads[core * x86_cpu_num_threads + thread] = 1;
			X86_THREAD.inst_mod->reachable_threads[core * x86_cpu_num_threads + thread] = 1;
			list_push(stack, X86_THREAD.data_mod);
			list_push(stack, X86_THREAD.inst_mod);
		}
	}

	/* Process pending modules */
	while (list_count(stack))
	{
		struct mod_t *mod = list_pop(stack);
		LINKED_LIST_FOR_EACH(mod->low_mod_list)
		{
			struct mod_t *low_mod = linked_list_get(mod->low_mod_list);
			int or = 0; /* For assertions only */
			for (int i = 0; i < total_num_threads; i++)
			{
				or |= mod->reachable_threads[i];
				low_mod->reachable_threads[i] |= mod->reachable_threads[i];
			}
			assert(or); /* At least is reachable by one thread */

			list_push(stack, low_mod);
		}
	}

	/* Count reachable threads per module */
	LIST_FOR_EACH(mem_system->mod_list, m)
	{
		struct mod_t *mod = list_get(mem_system->mod_list, m);
		for (int i = 0; i < total_num_threads; i++)
		{
			if (mod->reachable_threads[i])
				mod->num_reachable_threads++;
		}
	}

	list_free(stack);
}


/* Set in each thread the modules that are reachable per level */
static void mem_config_mod_reachability()
{
	int i;
	int core;
	int thread;

	if (arch_x86->sim_kind != arch_sim_kind_detailed)
		return;

	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int thread_id = core * x86_cpu_num_threads + thread;

		/* Create structures. They are destroyed when the thread is freed. */
		X86_THREAD.reachable_modules_per_level = xcalloc(max_mod_level + 1, sizeof(struct list_t *));
		for (int level = 1; level <= max_mod_level; level++)
			X86_THREAD.reachable_modules_per_level[level] = list_create();

		LIST_FOR_EACH(mem_system->mod_list, i)
		{
			struct mod_t *mod = list_get(mem_system->mod_list, i);

			assert(mod);
			assert(mod->level > 0 && mod->level <= max_mod_level);

			if (mod->reachable_threads[thread_id])
				list_push(X86_THREAD.reachable_modules_per_level[mod->level], mod);
		}
	}
}


void mem_config_create_atds()
{
	int i;

	LIST_FOR_EACH(mem_system->mod_list, i)
	{
		struct mod_t *mod = list_get(mem_system->mod_list, i);
		int core;
		int thread;
		X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
		{
			int thread_id = core * x86_cpu_num_threads + thread;
			if (mod->reachable_threads[thread_id])
				mod->atd_per_thread[thread_id] = atd_create(mod, mod->cache->num_sets); /* TODO: For now, all sets are monitored */
		}
	}
}


void mem_config_cache_partitioning()
{
	int i;
	struct mod_t *mod;
	struct cache_partitioning_t *wrapper;

	LIST_FOR_EACH(mem_system->mod_list, i)
	{
		mod = list_get(mem_system->mod_list, i);
		if (mod->kind != mod_kind_cache)
			continue;
		switch (mod->cache->partitioning.policy)
		{
			case cache_partitioning_policy_none:
				break;
			case cache_partitioning_policy_static:
				fatal("Static not implemented: %s", __FUNCTION__);
				break;
			case cache_partitioning_policy_ucp:
				wrapper = cache_partitioning_create(mod, ucp_create, ucp_free, ucp_repartition);
				cache_partitioning_schedule(wrapper);
				break;
			case cache_partitioning_policy_fcp:
				wrapper = cache_partitioning_create(mod, fcp_create, fcp_free, fcp_repartition);
				cache_partitioning_schedule(wrapper);
				break;
			default:
				fatal("Partitioning policy not implemented: %s", __FUNCTION__);
				break;
		}
	}
}


/*
 * Public Functions
 */


void mem_config_read(void)
{
	struct config_t *config;

	/* Load memory system configuration file. If no file name has been given
	 * by the user, create a default configuration for each architecture. */
	config = config_create(mem_config_file_name);
	config_set_interpolation(config, 1);

	if (!*mem_config_file_name)
		arch_for_each(mem_config_default, config);
	else
		config_load(config);

	/* Read general variables */
	mem_config_read_general(config);

	/* Read networks */
	mem_config_read_networks(config);

	/* Read main memory systems */
	mem_config_read_dram_systems(config);

	/* Read modules */
	mem_config_read_modules(config);

	/* Read low level caches */
	mem_config_read_low_modules(config);

	/* Read entries from requesting devices (CPUs/GPUs) to memory system entries.
	 * This is presented in [Entry <name>] sections in the configuration file. */
	mem_config_read_entries(config);

	/* Create switches in internal networks */
	mem_config_create_switches(config);

	/* Read commands from the configuration file. Commands are used to artificially
	 * alter the initial state of the memory hierarchy for debugging purposes. */
	mem_config_read_commands(config);

	/* Check that all enforced sections and variables were specified */
	config_check(config);
	config_free(config);

	/* Check routes to low and high modules */
	mem_config_check_routes();

	/* Check for disjoint memory hierarchies for different architectures. */
	if (!si_gpu_fused_device)
		arch_for_each(mem_config_check_disjoint, NULL);

	/* Compute sub-block sizes, based on high modules. This function also
	 * initializes the directories in modules other than L1. */
	mem_config_calculate_sub_block_sizes();

	/* Compute cache levels relative to the CPU/GPU entry points */
	mem_config_calculate_mod_levels();

	/* Compute wich threads can access a given memory module */
	mem_config_x86_thread_reachability();

	/* Compute wich dram systems are accessible from a given memory module */
	mem_config_main_memory_reachability();

	/* Compute for each thread the modules that are reachable per level */
	mem_config_mod_reachability();

	/* Create ATDs for each thread accessig each cache */
	mem_config_create_atds();

	/* Prepare structures and schedule events */
	mem_config_cache_partitioning();

	/* Dump configuration to trace file */
	mem_config_trace();
}

