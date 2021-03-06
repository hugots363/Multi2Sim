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

#ifndef SOUTHERN_ISLANDS_TIMING_H
#define SOUTHERN_ISLANDS_TIMING_H

#include <limits.h>

#include <arch/southern-islands/emu/emu.h>
#include <mem-system/mem-system.h>

#define INST_NOT_FETCHED LLONG_MAX

/*
 * GPU uop
 */

/* Debugging */
#define si_stack_debug(...) debug(si_stack_debug_category, __VA_ARGS__)
extern int si_stack_debug_category;


/* Part of a GPU instruction specific for each work-item within wavefront. */
struct si_work_item_uop_t
{
	/* For global memory accesses */
	uint32_t global_mem_access_addr;
	uint32_t global_mem_access_size;

	/* Flags */
	unsigned int active : 1;  /* Active after instruction emulation */

	/* Local memory access */
	int local_mem_access_count;
	enum mod_access_kind_t local_mem_access_kind[SI_MAX_LOCAL_MEM_ACCESSES_PER_INST];
	uint32_t local_mem_access_addr[SI_MAX_LOCAL_MEM_ACCESSES_PER_INST];
	uint32_t local_mem_access_size[SI_MAX_LOCAL_MEM_ACCESSES_PER_INST];
};

/* Structure representing a GPU instruction fetched in common for a wavefront.
 * This is the structure passed from stage to stage in the compute unit pipeline. */
struct si_uop_t
{
	/* Fields */
	long long id;
	long long id_in_compute_unit;
	int inst_buffer_id;
	struct si_wavefront_t *wavefront;        /* Wavefront it belongs to */
	struct si_work_group_t *work_group;      /* Work-group it belongs to */
	struct si_compute_unit_t *compute_unit;  /* Compute unit it belongs to */
	struct si_inst_buffer_entry_t *inst_buffer_entry;  /* IB entry where uop is located */
	struct si_inst_t inst;

	/* Flags */
	unsigned int ready : 1;
	unsigned int mem_wait_inst : 1;
	unsigned int barrier_wait_inst : 1;
	unsigned int wavefront_last_inst : 1;   /* Last instruction in the wavefront */
	unsigned int vector_mem_read : 1;
	unsigned int vector_mem_write : 1;
	unsigned int scalar_mem_read : 1;
	unsigned int local_mem_read : 1;
	unsigned int local_mem_write : 1;
	unsigned int exec_mask_update : 1;

	/* Timing */
	long long fetch_ready;      /* Cycle when fetch completes */
	long long decode_ready;     /* Cycle when decode completes */
	long long read_ready;       /* Cycle when register access completes */
	long long execute_ready;    /* Cycle when execution completes */
	long long writeback_ready;  /* Cycle when writeback completes */

	/* Witness memory accesses */
	int global_mem_witness;
	int local_mem_witness;
	
	/* Last scalar memory accesses */
	uint32_t global_mem_access_addr;
	uint32_t global_mem_access_size;

	/* Per stream-core data. This space is dynamically allocated for an uop.
	 * It should be always the last field of the structure. */
	struct si_work_item_uop_t work_item_uop[0];
};


void si_uop_init(void);
void si_uop_done(void);

struct si_uop_t *si_uop_create(void);
void si_uop_free(struct si_uop_t *gpu_uop);

void si_uop_list_free(struct linked_list_t *gpu_uop_list);
void si_uop_dump_dep_list(char *buf, int size, int *dep_list, int dep_count);

void si_uop_save_exec_mask(struct si_uop_t *uop);
void si_uop_debug_exec_mask(struct si_uop_t *uop);




/*
 * Register file in Compute Unit
 */

struct si_reg_file_t;

void si_reg_file_init(struct si_compute_unit_t *compute_unit);
void si_reg_file_done(struct si_compute_unit_t *compute_unit);

void si_reg_file_map_work_group(struct si_compute_unit_t *compute_unit,
	struct si_work_group_t *work_group);
void si_reg_file_unmap_work_group(struct si_compute_unit_t *compute_unit,
	struct si_work_group_t *work_group);

int si_reg_file_rename(struct si_compute_unit_t *compute_unit,
	struct si_work_item_t *work_item, int logical_register);
void si_reg_file_inverse_rename(struct si_compute_unit_t *compute_unit,
	int physical_register, struct si_work_item_t **work_item, int *logical_register);



/*
 * Instruction Buffer 
 */

struct si_inst_buffer_entry_t 
{
	unsigned int valid : 1; /* Valid if wavefront assigned to entry */

	int id_in_inst_buffer;
	struct si_inst_buffer_t *inst_buffer;

	struct si_wavefront_t *wavefront;
	long long int cycle_fetched;
	struct si_uop_t *uop;

	/* Status (not mutually exclusive) */
	unsigned int ready : 1;              /* Ready to fetch next instruction */
	/* TOOD Break wait_for_mem into waiting for each memory type */
	unsigned int wait_for_mem : 1;       /* Waiting for memory instructions */
	unsigned int wait_for_barrier : 1;   /* Waiting at barrier for other wavefronts */
	unsigned int wavefront_finished : 1; /* Wavefront has executed last instruction */

	/* Outstanding memory accesses */
	unsigned int vm_cnt;     /* Vector memory count */
	unsigned int exp_cnt;    /* Export count */
	unsigned int lgkm_cnt;   /* LDS, GDS, Constant, and message count */
};

struct si_inst_buffer_t
{
	int id;

	/* List of currently mapped wavefronts */
	int wavefront_count;
	struct si_inst_buffer_entry_t **entries;

	/* Compute unit */
	struct si_compute_unit_t *compute_unit;
};


/* 
 * Hardware Units 
 */

struct si_branch_unit_t
{
	/* Queues */
	struct list_t *decode_buffer; /* Decoded instructions */
	struct list_t *read_buffer;   /* Register accesses */
	struct list_t *exec_buffer;   /* Execution */

	struct si_compute_unit_t *compute_unit;

	/* Statistics */
	long long inst_count;
};

struct si_scalar_unit_t
{
	struct list_t *decode_buffer;   /* Decoded instructions */
	struct list_t *read_buffer;     /* Register accesses */
	struct list_t *exec_buffer;     /* Execution (both ALU and MEM) */
	struct list_t *inflight_buffer; /* Pending memory accesses */

	struct si_compute_unit_t *compute_unit;

	/* Statistics */
	long long inst_count;
};

struct si_vector_mem_unit_t
{
	struct list_t *decode_buffer;   /* Decoded instructions */
	struct list_t *read_buffer;     /* Register accesses */
	struct list_t *exec_buffer;     /* Submitted memory accesses */
	struct list_t *inflight_buffer; /* Pending for memory access */

	struct si_compute_unit_t *compute_unit;

	/* Statistics */
	long long inst_count;
};

struct si_subwavefront_pool_t
{
	struct si_uop_t *uop;
	unsigned int num_subwavefronts_executed;
};

struct si_simd_t
{
	struct list_t *decode_buffer; /* Decoded instructions */
	struct list_t *read_buffer;   /* Register accesses */
	struct list_t *exec_buffer;   /* Execution */
	struct si_subwavefront_pool_t
		*subwavefront_pool;		  /* Keep track of the wavefront entering
									 stream core pipelines. */

	struct si_compute_unit_t *compute_unit;

	/* Statistics */
	long long inst_count;
};

struct si_lds_t
{
	struct list_t *decode_buffer;   /* Decoded instructions */
	struct list_t *read_buffer;     /* Register accesses */
	struct list_t *exec_buffer;     /* Submitted memory accesses */
	struct list_t *inflight_buffer; /* Pending for memory access */

	struct si_compute_unit_t *compute_unit;

	/* Statistics */
	long long inst_count;
};


/*
 * GPU Compute Unit
 */

struct si_compute_unit_t
{
	/* IDs */
	int id;
	long long uop_id_counter;  /* Counter to assign 'id_in_compute_unit' to uops */
	long long mem_uop_id_counter;  /* Counter to assign 'id_in_compute_unit' to mem uops */

	/* Double linked list of compute units */
	struct si_compute_unit_t *compute_unit_ready_list_prev;
	struct si_compute_unit_t *compute_unit_ready_list_next;
	struct si_compute_unit_t *compute_unit_busy_list_prev;
	struct si_compute_unit_t *compute_unit_busy_list_next;

	/* Entry points to memory hierarchy */
	struct mod_t *global_memory;
	struct mod_t *local_memory;

	/* Hardware structures */
	unsigned int num_inst_buffers;
	struct si_inst_buffer_t **inst_buffers;
	struct si_simd_t **simds;
	/* TODO Make these into a configurable number of structures */
	struct si_scalar_unit_t scalar_unit;
	struct si_branch_unit_t branch_unit;
	struct si_vector_mem_unit_t vector_mem_unit;
	struct si_lds_t lds;

	/* Statistics */
	long long cycle;
	long long mapped_work_groups;
	long long wavefront_count;
	long long inst_count; /* Total instructions */
	long long branch_inst_count;
	long long scalar_alu_inst_count;
	long long scalar_mem_inst_count;
	/* TODO Have one SIMD inst count per SIMD unit */
	long long simd_inst_count;
	long long vector_mem_inst_count;
	long long local_mem_inst_count;

	/* List of currently mapped work-groups */
	int work_group_count;
	struct si_work_group_t **work_groups;
};

struct si_compute_unit_t *si_compute_unit_create(void);
void si_compute_unit_free(struct si_compute_unit_t *gpu_compute_unit);
void si_compute_unit_map_work_group(struct si_compute_unit_t *compute_unit, 
	struct si_work_group_t *work_group);
void si_compute_unit_unmap_work_group(struct si_compute_unit_t *compute_unit, 
	struct si_work_group_t *work_group);
struct si_wavefront_t *si_compute_unit_schedule(struct si_compute_unit_t *compute_unit);
void si_compute_unit_run(struct si_compute_unit_t *compute_unit);

struct si_inst_buffer_t *si_inst_buffer_create();
void si_inst_buffer_free(struct si_inst_buffer_t *inst_buffer);
void si_inst_buffer_map_wavefronts(struct si_inst_buffer_t *inst_buffer, 
	struct si_work_group_t *work_group);
void si_inst_buffer_unmap_wavefronts(struct si_inst_buffer_t *inst_buffer, 
	struct si_work_group_t *work_group);


/*
 * GPU Calculator
 */

int si_calc_get_work_groups_per_inst_buffer(int work_items_per_work_group,
	int registers_per_work_item, int local_mem_per_work_group);
void si_calc_plot(void);



/*
 * Southern Islands GPU
 */

/* Trace */
#define si_tracing() trace_status(si_trace_category)
#define si_trace(...) trace(si_trace_category, __VA_ARGS__)
#define si_trace_header(...) trace_header(si_trace_category, __VA_ARGS__)
extern int si_trace_category;

extern char *si_gpu_config_help;
extern char *si_gpu_config_file_name;
extern char *si_gpu_report_file_name;

/* OpenCL Device Info */
extern unsigned int si_gpu_device_type;
extern unsigned int si_gpu_device_vendor_id;

extern char *si_gpu_device_profile;
extern char *si_gpu_device_name;
extern char *si_gpu_device_vendor;
extern char *si_gpu_device_extensions;
extern char *si_gpu_device_version;
extern char *si_gpu_driver_version;
extern char *si_gpu_opencl_version;

extern unsigned int si_gpu_work_group_size;
extern unsigned int si_gpu_work_item_dimensions;
extern unsigned int si_gpu_work_item_sizes[3];

extern unsigned int si_gpu_image_support;
extern unsigned int si_gpu_max_read_image_args;
extern unsigned int si_gpu_max_write_image_args;

extern unsigned int si_gpu_image2d_max_width;
extern unsigned int si_gpu_image2d_max_height;
extern unsigned int si_gpu_image3d_max_width;
extern unsigned int si_gpu_image3d_max_height;
extern unsigned int si_gpu_image3d_max_depth;
extern unsigned int si_gpu_max_samplers;

extern unsigned int si_gpu_max_parameter_size;
extern unsigned int si_gpu_mem_base_addr_align;
extern unsigned int si_gpu_min_data_type_align_size;

extern unsigned int si_gpu_single_fp_config;
extern unsigned int si_gpu_double_fp_config;

extern unsigned int si_gpu_max_clock_frequency;
extern unsigned int si_gpu_address_bits;

extern unsigned int si_gpu_global_mem_cache_type;
extern unsigned int si_gpu_global_mem_cacheline_size;
extern unsigned long long si_gpu_global_mem_cache_size;
extern unsigned long long si_gpu_global_mem_size;
extern unsigned long long si_gpu_max_mem_alloc_size;

extern unsigned int si_gpu_local_mem_type;

extern unsigned long long si_gpu_max_constant_buffer_size;
extern unsigned int si_gpu_max_constant_args;

extern unsigned int si_gpu_vector_width_half;

extern unsigned int si_gpu_error_correction_support;
extern unsigned int si_gpu_host_unified_memory;
extern unsigned int si_gpu_profiling_timer_resolution;
extern unsigned int si_gpu_endian_little;

extern unsigned int si_gpu_device_available;
extern unsigned int si_gpu_compiler_available;
extern unsigned int si_gpu_linker_available;
extern unsigned int si_gpu_execution_capabilities;

extern unsigned int si_gpu_queue_properties;

extern unsigned int si_gpu_platform;

extern unsigned int si_gpu_num_compute_units;
extern unsigned int si_gpu_num_registers;
extern unsigned int si_gpu_num_inst_buffers;
extern unsigned int si_gpu_num_stream_cores;
extern unsigned int si_gpu_register_alloc_size;

extern struct str_map_t si_gpu_register_alloc_granularity_map;
extern enum si_gpu_register_alloc_granularity_t
{
	si_gpu_register_alloc_invalid = 0,  /* For invalid user input */
	si_gpu_register_alloc_wavefront,
	si_gpu_register_alloc_work_group
} si_gpu_register_alloc_granularity;

extern int si_gpu_max_work_groups_per_inst_buffer;
extern int si_gpu_max_wavefronts_per_inst_buffer;

extern struct str_map_t si_gpu_sched_policy_map;
extern enum si_gpu_sched_policy_t
{
	si_gpu_sched_invalid = 0,  /* For invalid user input */
	si_gpu_sched_round_robin,
	si_gpu_sched_greedy
} si_gpu_sched_policy;

extern char *si_gpu_calc_file_name;

extern int si_gpu_fetch_latency;

extern int si_gpu_decode_latency;
extern int si_gpu_decode_width;

extern int si_gpu_local_mem_size;
extern int si_gpu_local_mem_alloc_size;
extern int si_gpu_local_mem_latency;
extern int si_gpu_local_mem_block_size;
extern int si_gpu_local_mem_num_ports;

extern int si_gpu_simd_width;
extern int si_gpu_simd_decode_buffer_size;
extern int si_gpu_simd_read_latency;
extern int si_gpu_simd_read_buffer_size;
extern int si_gpu_simd_alu_latency;
extern int si_gpu_simd_num_subwavefronts;

extern int si_gpu_vector_mem_width;
extern int si_gpu_vector_mem_decode_buffer_size;
extern int si_gpu_vector_mem_read_latency;
extern int si_gpu_vector_mem_read_buffer_size;
extern int si_gpu_vector_mem_inflight_mem_accesses;

extern int si_gpu_lds_width;
extern int si_gpu_lds_decode_buffer_size;
extern int si_gpu_lds_read_latency;
extern int si_gpu_lds_read_buffer_size;
extern int si_gpu_lds_inflight_mem_accesses;

extern int si_gpu_scalar_unit_width;
extern int si_gpu_scalar_unit_decode_buffer_size;
extern int si_gpu_scalar_unit_read_latency;
extern int si_gpu_scalar_unit_read_buffer_size;
extern int si_gpu_scalar_unit_exec_latency;
extern int si_gpu_scalar_unit_inflight_mem_accesses;

extern int si_gpu_branch_unit_width;
extern int si_gpu_branch_unit_decode_buffer_size;
extern int si_gpu_branch_unit_read_latency;
extern int si_gpu_branch_unit_read_buffer_size;
extern int si_gpu_branch_unit_exec_latency;

struct si_gpu_t
{
	/* Current cycle */
	long long cycle;

	/* ND-Range running on it */
	struct si_ndrange_t *ndrange;
	int work_groups_per_inst_buffer;
	int wavefronts_per_inst_buffer;
	int work_items_per_inst_buffer;
	int work_groups_per_compute_unit;
	int wavefronts_per_compute_unit;
	int work_items_per_compute_unit;

	/* Compute units */
	struct si_compute_unit_t **compute_units;

	/* List of ready compute units accepting work-groups */
	struct si_compute_unit_t *compute_unit_ready_list_head;
	struct si_compute_unit_t *compute_unit_ready_list_tail;
	int compute_unit_ready_list_count;
	int compute_unit_ready_list_max;

	/* List of busy compute units */
	struct si_compute_unit_t *compute_unit_busy_list_head;
	struct si_compute_unit_t *compute_unit_busy_list_tail;
	int compute_unit_busy_list_count;
	int compute_unit_busy_list_max;

	/* List of deleted instructions */
	struct linked_list_t *trash_uop_list;

	long long int last_complete_cycle;
};

extern struct si_gpu_t *si_gpu;

#define SI_GPU_FOREACH_COMPUTE_UNIT(COMPUTE_UNIT_ID) \
	for ((COMPUTE_UNIT_ID) = 0; (COMPUTE_UNIT_ID) < si_gpu_num_compute_units; (COMPUTE_UNIT_ID)++)

#define SI_GPU_FOREACH_WORK_ITEM_IN_SUBWAVEFRONT(WAVEFRONT, SUBWAVEFRONT_ID, WORK_ITEM_ID) \
	for ((WORK_ITEM_ID) = (WAVEFRONT)->work_item_id_first + (SUBWAVEFRONT_ID) * si_gpu_num_stream_cores; \
		(WORK_ITEM_ID) <= MIN((WAVEFRONT)->work_item_id_first + ((SUBWAVEFRONT_ID) + 1) \
			* si_gpu_num_stream_cores - 1, (WAVEFRONT)->work_item_id_last); \
		(WORK_ITEM_ID)++)

void si_gpu_init(void);
void si_gpu_done(void);

void si_gpu_dump_report(void);
void si_gpu_dump_summary(FILE *f);

void si_gpu_uop_trash_add(struct si_uop_t *uop);
void si_gpu_uop_trash_empty(void);

void si_compute_unit_run_simd(struct si_compute_unit_t *compute_unit);
void si_compute_unit_run_scalar_unit(struct si_compute_unit_t *compute_unit);
void si_compute_unit_run_branch_unit(struct si_compute_unit_t *compute_unit);
int si_gpu_run(void);

void si_simd_run(struct si_simd_t *simd);
void si_scalar_unit_run(struct si_scalar_unit_t *scalar_unit);
void si_branch_unit_run(struct si_branch_unit_t *branch_unit);
void si_vector_mem_run(struct si_vector_mem_unit_t *vector_mem);
void si_lds_run(struct si_lds_t *lds);

#endif
