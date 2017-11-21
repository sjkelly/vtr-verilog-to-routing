#include <cstdio>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>
using namespace std;

#include "vtr_assert.h"
#include "vtr_util.h"
#include "vtr_log.h"
#include "vtr_digest.h"
#include "vtr_memory.h"

#include "vpr_types.h"
#include "vpr_error.h"
#include "vpr_utils.h"

#include "stats.h"
#include "globals.h"
#include "route_export.h"
#include "route_common.h"
#include "route_tree_timing.h"
#include "route_timing.h"
#include "route_breadth_first.h"
#include "place_and_route.h"
#include "build_rr_graph.h"
#include "build_rr_graph2.h"
#include "read_xml_arch_file.h"
#include "draw.h"
#include "echo_files.h"

#include "route_profiling.h"

#include "timing_util.h"
#include "RoutingDelayCalculator.h"
#include "timing_info.h"
#include "tatum/echo_writer.hpp"


#include "path_delay.h"
 

/**************** Static variables local to route_common.c ******************/

static t_heap **heap; /* Indexed from [1..heap_size] */
static int heap_size; /* Number of slots in the heap array */
static int heap_tail; /* Index of first unused slot in the heap array */

/* For managing my own list of currently free heap data structures.     */
static t_heap *heap_free_head = NULL;
/* For keeping track of the sudo malloc memory for the heap*/
static vtr::t_chunk heap_ch;

/* For managing my own list of currently free trace data structures.    */
static t_trace *trace_free_head = NULL;
/* For keeping track of the sudo malloc memory for the trace*/
static vtr::t_chunk trace_ch;

static int num_trace_allocated = 0; /* To watch for memory leaks. */
static int num_heap_allocated = 0;
static int num_linked_f_pointer_allocated = 0;

static t_linked_f_pointer *rr_modified_head = NULL;
static t_linked_f_pointer *linked_f_pointer_free_head = NULL;

static vtr::t_chunk linked_f_pointer_ch;

/*  The numbering relation between the channels and clbs is:				*
 *																	        *
 *  |    IO     | chan_   |   CLB     | chan_   |   CLB     |               *
 *  |grid[0][2] | y[0][2] |grid[1][2] | y[1][2] | grid[2][2]|               *
 *  +-----------+         +-----------+         +-----------+               *
 *                                                            } capacity in *
 *   No channel           chan_x[1][1]          chan_x[2][1]  } chan_width  *
 *                                                            } _x[1]       *
 *  +-----------+         +-----------+         +-----------+               *
 *  |           |  chan_  |           |  chan_  |           |               *
 *  |    IO     | y[0][1] |   CLB     | y[1][1] |   CLB     |               *
 *  |grid[0][1] |         |grid[1][1] |         |grid[2][1] |               *
 *  |           |         |           |         |           |               *
 *  +-----------+         +-----------+         +-----------+               *
 *                                                            } capacity in *
 *                        chan_x[1][0]          chan_x[2][0]  } chan_width  * 
 *                                                            } _x[0]       *
 *                        +-----------+         +-----------+               *
 *                 No     |           |	   No   |           |               *
 *               Channel  |    IO     | Channel |    IO     |               *
 *                        |grid[1][0] |         |grid[2][0] |               *
 *                        |           |         |           |               *
 *                        +-----------+         +-----------+               *
 *                                                                          *
 *               {=======}              {=======}                           *
 *              Capacity in            Capacity in                          *
 *            chan_width_y[0]        chan_width_y[1]                        *
 *                                                                          */

/******************** Subroutines local to route_common.c *******************/


static void add_to_heap(t_heap *hptr);
static t_heap *alloc_heap_data(void);
static t_linked_f_pointer *alloc_linked_f_pointer(void);

static vtr::vector_map<ClusterNetId, std::vector<int>> load_net_rr_terminals(const t_rr_node_indices& L_rr_node_indices);
static vtr::vector_map<ClusterNetId, t_bb> load_route_bb(int bb_factor);
static vtr::vector_map<ClusterBlockId, std::vector<int>> load_rr_clb_sources(const t_rr_node_indices& L_rr_node_indices);

static t_clb_opins_used alloc_and_load_clb_opins_used_locally(void);
static void adjust_one_rr_occ_and_apcost(int inode, int add_or_sub,
		float pres_fac, float acc_fac);

/************************** Subroutine definitions ***************************/

void save_routing(vtr::vector_map<ClusterNetId, t_trace *> &best_routing,
		const t_clb_opins_used& clb_opins_used_locally,
		t_clb_opins_used& saved_clb_opins_used_locally) {

	/* This routing frees any routing currently held in best routing,       *
	 * then copies over the current routing (held in route_ctx.trace_head), and       *
	 * finally sets route_ctx.trace_head and route_ctx.trace_tail to all NULLs so that the      *
	 * connection to the saved routing is broken.  This is necessary so     *
	 * that the next iteration of the router does not free the saved        *
	 * routing elements.  Also saves any data about locally used clb_opins, *
	 * since this is also part of the routing.                              */

	t_trace *tptr, *tempptr;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	for (auto net_id : cluster_ctx.clb_nlist.nets()) {

		/* Free any previously saved routing.  It is no longer best. */
		tptr = best_routing[net_id];
		while (tptr != NULL) {
			tempptr = tptr->next;
			free_trace_data(tptr);
			tptr = tempptr;
		}

		/* Save a pointer to the current routing in best_routing. */
		best_routing[net_id] = route_ctx.trace_head[net_id];

		/* Set the current (working) routing to NULL so the current trace       *
		 * elements won't be reused by the memory allocator.                    */

		route_ctx.trace_head[net_id] = NULL;
		route_ctx.trace_tail[net_id] = NULL;
	}

	/* Save which OPINs are locally used.                           */
    saved_clb_opins_used_locally = clb_opins_used_locally;
}

/* Deallocates any current routing in route_ctx.trace_head, and replaces it with    *
	 * the routing in best_routing.  Best_routing is set to NULL to show that *
	 * it no longer points to a valid routing.  NOTE:  route_ctx.trace_tail is not      *
	 * restored -- it is set to all NULLs since it is only used in            *
	 * update_traceback.  If you need route_ctx.trace_tail restored, modify this        *
	 * routine.  Also restores the locally used opin data.                    */
void restore_routing(vtr::vector_map<ClusterNetId, t_trace *> &best_routing,
		t_clb_opins_used&  clb_opins_used_locally,
		const t_clb_opins_used&  saved_clb_opins_used_locally) {
	
    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	for (auto net_id : cluster_ctx.clb_nlist.nets()) {
		/* Free any current routing. */
		free_traceback(net_id);

		/* Set the current routing to the saved one. */
		route_ctx.trace_head[net_id] = best_routing[net_id];
		best_routing[net_id] = NULL; /* No stored routing. */
	}

	/* Restore which OPINs are locally used.                           */
    clb_opins_used_locally = saved_clb_opins_used_locally;
}

/* This routine finds a "magic cookie" for the routing and prints it.    *
* Use this number as a routing serial number to ensure that programming *
* changes do not break the router.                                      */
void get_serial_num(void) {
	int serial_num, inode;
	t_trace *tptr;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.routing();
    auto& device_ctx = g_vpr_ctx.device();

	serial_num = 0;

	for (auto net_id : cluster_ctx.clb_nlist.nets()) {

		/* Global nets will have null trace_heads (never routed) so they *
		 * are not included in the serial number calculation.            */

		tptr = route_ctx.trace_head[net_id];
		while (tptr != NULL) {
			inode = tptr->index;
			serial_num += (size_t(net_id) + 1)
					* (device_ctx.rr_nodes[inode].xlow() * (device_ctx.grid.width()) - device_ctx.rr_nodes[inode].yhigh());

			serial_num -= device_ctx.rr_nodes[inode].ptc_num() * (size_t(net_id) + 1) * 10;

			serial_num -= device_ctx.rr_nodes[inode].type() * (size_t(net_id) + 1) * 100;
			serial_num %= 2000000000; /* Prevent overflow */
			tptr = tptr->next;
		}
	}
	vtr::printf_info("Serial number (magic cookie) for the routing is: %d\n", serial_num);
}

void try_graph(int width_fac, t_router_opts router_opts,
		t_det_routing_arch *det_routing_arch, t_segment_inf * segment_inf,
		t_chan_width_dist chan_width_dist,
		t_direct_inf *directs, int num_directs) {

    auto& device_ctx = g_vpr_ctx.mutable_device();

	t_graph_type graph_type;
	if (router_opts.route_type == GLOBAL) {
		graph_type = GRAPH_GLOBAL;
	} else {
		graph_type = (det_routing_arch->directionality == BI_DIRECTIONAL ?
						GRAPH_BIDIR : GRAPH_UNIDIR);
	}

	/* Set the channel widths */
	init_chan(width_fac, chan_width_dist);

	/* Free any old routing graph, if one exists. */
	free_rr_graph();

	clock_t begin = clock();

	/* Set up the routing resource graph defined by this FPGA architecture. */
	int warning_count;
	device_ctx.rr_graph = create_rr_graph(graph_type,
                            device_ctx.num_block_types, device_ctx.block_types,
                            device_ctx.grid,
                            &device_ctx.chan_width,
                            device_ctx.num_arch_switches, 
                            det_routing_arch,
                            segment_inf,
                            router_opts.base_cost_type, 
                            router_opts.trim_empty_channels,
                            router_opts.trim_obs_channels,
                            directs, num_directs,
                            &device_ctx.num_rr_switches,
                            &warning_count);

	clock_t end = clock();

	vtr::printf_info("Build rr_graph took %g seconds.\n", (float)(end - begin) / CLOCKS_PER_SEC);
}

bool try_route(int width_fac, t_router_opts router_opts,
		t_det_routing_arch *det_routing_arch, t_segment_inf * segment_inf,
		vtr::vector_map<ClusterNetId, float *> &net_delay,
#ifdef ENABLE_CLASSIC_VPR_STA
        t_slack * slacks,
        const t_timing_inf& timing_inf,
#endif
        std::shared_ptr<SetupHoldTimingInfo> timing_info,
		t_chan_width_dist chan_width_dist,
		t_direct_inf *directs, int num_directs,
        ScreenUpdatePriority first_iteration_priority) {

	/* Attempts a routing via an iterated maze router algorithm.  Width_fac *
	 * specifies the relative width of the channels, while the members of   *
	 * router_opts determine the value of the costs assigned to routing     *
	 * resource node, etc.  det_routing_arch describes the detailed routing *
	 * architecture (connection and switch boxes) of the FPGA; it is used   *
	 * only if a DETAILED routing has been selected.                        */

    auto& device_ctx = g_vpr_ctx.mutable_device();
    auto& cluster_ctx = g_vpr_ctx.clustering();

	t_graph_type graph_type;
	if (router_opts.route_type == GLOBAL) {
		graph_type = GRAPH_GLOBAL;
	} else {
		graph_type = (det_routing_arch->directionality == BI_DIRECTIONAL ?
						GRAPH_BIDIR : GRAPH_UNIDIR);
	}

	/* Set the channel widths */
	init_chan(width_fac, chan_width_dist);

	/* Free any old routing graph, if one exists. */
	free_rr_graph();

	clock_t begin = clock();

	/* Set up the routing resource graph defined by this FPGA architecture. */
	int warning_count;
        
	device_ctx.rr_graph = create_rr_graph(graph_type,
                            device_ctx.num_block_types, device_ctx.block_types,
                            device_ctx.grid,
                            &device_ctx.chan_width,
                            device_ctx.num_arch_switches, 
                            det_routing_arch,
                            segment_inf,
                            router_opts.base_cost_type, 
                            router_opts.trim_empty_channels,
                            router_opts.trim_obs_channels,
                            directs, num_directs,
                            &device_ctx.num_rr_switches,
                            &warning_count);

	clock_t end = clock();

	vtr::printf_info("Build rr_graph took %g seconds.\n", (float)(end - begin) / CLOCKS_PER_SEC);

    //Initialize drawing, now that we have an RR graph
    init_draw_coords(width_fac);

	bool success = true;

	/* Allocate and load additional rr_graph information needed only by the router. */
	alloc_and_load_rr_node_route_structs();

	init_route_structs(router_opts.bb_factor);

    if (cluster_ctx.clb_nlist.nets().empty()) {
        vtr::printf_warning(__FILE__, __LINE__, "No nets to route\n");
    }

	if (router_opts.router_algorithm == BREADTH_FIRST) {
		vtr::printf_info("Confirming router algorithm: BREADTH_FIRST.\n");
		success = try_breadth_first_route(router_opts);
	} else { /* TIMING_DRIVEN route */
		vtr::printf_info("Confirming router algorithm: TIMING_DRIVEN.\n");

        IntraLbPbPinLookup intra_lb_pb_pin_lookup(device_ctx.block_types, device_ctx.num_block_types);


		success = try_timing_driven_route(
                    device_ctx.rr_graph, 
                    router_opts,
                    net_delay, 
                    intra_lb_pb_pin_lookup,
                    timing_info,
#ifdef ENABLE_CLASSIC_VPR_STA
                    slacks,
                    timing_inf,
#endif
                    first_iteration_priority);

		profiling::time_on_fanout_analysis();

	}

	return (success);
}

bool feasible_routing(void) {

	/* This routine checks to see if this is a resource-feasible routing.      *
	 * That is, are all rr_node capacity limitations respected?  It assumes    *
	 * that the occupancy arrays are up to date when it is called.             */

    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.routing();

	for (int inode = 0; inode < device_ctx.num_rr_nodes; inode++) {
		if (route_ctx.rr_node_route_inf[inode].occ() > device_ctx.rr_nodes[inode].capacity()) {
			return (false);
		}
	}

	return (true);
}

void pathfinder_update_path_cost(t_trace *route_segment_start,
		int add_or_sub, float pres_fac) {

	/* This routine updates the occupancy and pres_cost of the rr_nodes that are *
	 * affected by the portion of the routing of one net that starts at          *
	 * route_segment_start.  If route_segment_start is route_ctx.trace_head[net_id], the     *
	 * cost of all the nodes in the routing of net net_id are updated.  If         *
	 * add_or_sub is -1 the net (or net portion) is ripped up, if it is 1 the    *
	 * net is added to the routing.  The size of pres_fac determines how severly *
	 * oversubscribed rr_nodes are penalized.                                    */

	t_trace *tptr;

	tptr = route_segment_start;
	if (tptr == NULL) /* No routing yet. */
		return;

    auto& device_ctx = g_vpr_ctx.device();

	for (;;) {
		pathfinder_update_single_node_cost(tptr->index, add_or_sub, pres_fac);

		if (device_ctx.rr_nodes[tptr->index].type() == SINK) {
			tptr = tptr->next; /* Skip next segment. */
			if (tptr == NULL)
				break;
		}

		tptr = tptr->next;

	} /* End while loop -- did an entire traceback. */
}

void pathfinder_update_single_node_cost(int inode, int add_or_sub, float pres_fac) {

	/* Updates pathfinder's congestion cost by either adding or removing the 
	 * usage of a resource node. pres_cost is Pn in the Pathfinder paper.
	 * pres_cost is set according to the overuse that would result from having
	 * ONE MORE net use this routing node.     */

    auto& route_ctx = g_vpr_ctx.mutable_routing();
    auto& device_ctx = g_vpr_ctx.device();
    
	int occ = route_ctx.rr_node_route_inf[inode].occ() + add_or_sub;
	route_ctx.rr_node_route_inf[inode].set_occ(occ);
	// can't have negative occupancy
	VTR_ASSERT(occ >= 0);

	int	capacity = device_ctx.rr_nodes[inode].capacity();
	if (occ < capacity) {
		route_ctx.rr_node_route_inf[inode].pres_cost = 1.0;
	} else {
		route_ctx.rr_node_route_inf[inode].pres_cost = 1.0 + (occ + 1 - capacity) * pres_fac;
	}
}
void pathfinder_update_cost(float pres_fac, float acc_fac) {

	/* This routine recomputes the pres_cost and acc_cost of each routing        *
	 * resource for the pathfinder algorithm after all nets have been routed.    *
	 * It updates the accumulated cost to by adding in the number of extra       *
	 * signals sharing a resource right now (i.e. after each complete iteration) *
	 * times acc_fac.  It also updates pres_cost, since pres_fac may have        *
	 * changed.  THIS ROUTINE ASSUMES THE OCCUPANCY VALUES IN RR_NODE ARE UP TO  *
	 * DATE.                                                                     */

	int inode, occ, capacity;
    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	for (inode = 0; inode < device_ctx.num_rr_nodes; inode++) {
		occ = route_ctx.rr_node_route_inf[inode].occ();
		capacity = device_ctx.rr_nodes[inode].capacity();

		if (occ > capacity) {
			route_ctx.rr_node_route_inf[inode].acc_cost += (occ - capacity) * acc_fac;
                        route_ctx.rr_node_route_inf[inode].pres_cost = 1.0 + (occ + 1 - capacity) * pres_fac;
		}

		/* If occ == capacity, we don't need to increase acc_cost, but a change    *
		 * in pres_fac could have made it necessary to recompute the cost anyway.  */

		else if (occ == capacity) {
			route_ctx.rr_node_route_inf[inode].pres_cost = 1.0 + pres_fac;
		}
	}
}

void init_heap(const DeviceGrid& grid) {
    if (heap != nullptr) {
        vtr::free(heap + 1);
        heap = nullptr;
    }
	heap_size = (grid.width() -1 ) * (grid.height() - 1);
	heap = (t_heap **) vtr::malloc(heap_size * sizeof(t_heap *));
	heap--; /* heap stores from [1..heap_size] */
	heap_tail = 1;
}

/* Call this before you route any nets.  It frees any old traceback and   *
	 * sets the list of rr_nodes touched to empty.                            */
void init_route_structs(int bb_factor) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

    //Free any old tracebacks
	for (auto net_id : cluster_ctx.clb_nlist.nets())
		free_traceback(net_id);

    //Allocate new tracebacks
	route_ctx.trace_head.resize(cluster_ctx.clb_nlist.nets().size());
	route_ctx.trace_tail.resize(cluster_ctx.clb_nlist.nets().size());

    init_heap(device_ctx.grid);

    //Various look-ups
    route_ctx.net_rr_terminals = load_net_rr_terminals(device_ctx.rr_node_indices);
	route_ctx.route_bb = load_route_bb(bb_factor);
    route_ctx.rr_blk_source = load_rr_clb_sources(device_ctx.rr_node_indices);
	route_ctx.clb_opins_used_locally = alloc_and_load_clb_opins_used_locally();
    route_ctx.net_status.resize(cluster_ctx.clb_nlist.nets().size());

	/* Check that things that should have been emptied after the last routing *
	 * really were.                                                           */

	if (rr_modified_head != NULL) {
		vpr_throw(VPR_ERROR_ROUTE, __FILE__, __LINE__, 
			"in init_route_structs. List of modified rr nodes is not empty.\n");
	}

	if (heap_tail != 1) {
		vpr_throw(VPR_ERROR_ROUTE, __FILE__, __LINE__, 
			"in init_route_structs. Heap is not empty.\n");
	}
}

t_trace *
update_traceback(t_heap *hptr, ClusterNetId net_id) {

	/* This routine adds the most recently finished wire segment to the         *
	 * traceback linked list.  The first connection starts with the net SOURCE  *
	 * and begins at the structure pointed to by route_ctx.trace_head[net_id]. Each         *
	 * connection ends with a SINK.  After each SINK, the next connection       *
	 * begins (if the net has more than 2 pins).  The first element after the   *
	 * SINK gives the routing node on a previous piece of the routing, which is *
	 * the link from the existing net to this new piece of the net.             *
	 * In each traceback I start at the end of a path and trace back through    *
	 * its predecessors to the beginning.  I have stored information on the     *
	 * predecesser of each node to make traceback easy -- this sacrificies some *
	 * memory for easier code maintenance.  This routine returns a pointer to   *
	 * the first "new" node in the traceback (node not previously in trace).    */

	t_trace *tptr, *prevptr, *temptail, *ret_ptr;
	int inode;
	short iedge;

	t_rr_type rr_type;

    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	// hptr points to the end of a connection
	inode = hptr->index;
       
	rr_type = device_ctx.rr_nodes[inode].type();
	if (rr_type != SINK) {
		vpr_throw(VPR_ERROR_ROUTE, __FILE__, __LINE__, 
			"in update_traceback. Expected type = SINK (%d).\n"
			"\tGot type = %d while tracing back net %zu.\n", SINK, rr_type, size_t(net_id));
	}

	tptr = alloc_trace_data(); /* SINK on the end of the connection */
	tptr->index = inode;
	tptr->iswitch = OPEN;
	tptr->next = NULL;
	temptail = tptr; /* This will become the new tail at the end */
	/* of the routine.                          */

	/* Now do it's predecessor. */

	inode = hptr->u.prev_node;
	iedge = hptr->prev_edge;

	while (inode != NO_PREVIOUS) {
		prevptr = alloc_trace_data();
		prevptr->index = inode;
		prevptr->iswitch = device_ctx.rr_nodes[inode].edge_switch(iedge);
		prevptr->next = tptr;
		tptr = prevptr;

		iedge = route_ctx.rr_node_route_inf[inode].prev_edge;
		inode = route_ctx.rr_node_route_inf[inode].prev_node;
	}

	if (route_ctx.trace_tail[net_id] != NULL) {
		route_ctx.trace_tail[net_id]->next = tptr; /* Traceback ends with tptr */
		ret_ptr = tptr->next; /* First new segment.       */
	} else { /* This was the first "chunk" of the net's routing */
		route_ctx.trace_head[net_id] = tptr;
		ret_ptr = tptr; /* Whole traceback is new. */
	}

	route_ctx.trace_tail[net_id] = temptail;
	return (ret_ptr);
}

/* The routine sets the path_cost to HUGE_POSITIVE_FLOAT for  *
* all channel segments touched by previous routing phases.    */
void reset_path_costs(void) {
	t_linked_f_pointer *mod_ptr;
	int num_mod_ptrs;

	/* The traversal method below is slightly painful to make it faster. */
	if (rr_modified_head != NULL) {
		mod_ptr = rr_modified_head;

		num_mod_ptrs = 1;

		while (mod_ptr->next != NULL) {
			*(mod_ptr->fptr) = HUGE_POSITIVE_FLOAT;
			mod_ptr = mod_ptr->next;
			num_mod_ptrs++;
		}
		*(mod_ptr->fptr) = HUGE_POSITIVE_FLOAT; /* Do last one. */

		/* Reset the modified list and put all the elements back in the free   *
		 * list.                                                               */

		mod_ptr->next = linked_f_pointer_free_head;
		linked_f_pointer_free_head = rr_modified_head;
		rr_modified_head = NULL;

		num_linked_f_pointer_allocated -= num_mod_ptrs;
	}
}

/* Returns the *congestion* cost of using this rr_node. */
float get_rr_cong_cost(int inode) {
	short cost_index;
	float cost;

    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.routing();

	cost_index = device_ctx.rr_nodes[inode].cost_index();
	cost = device_ctx.rr_indexed_data[cost_index].base_cost
			* route_ctx.rr_node_route_inf[inode].acc_cost
			* route_ctx.rr_node_route_inf[inode].pres_cost;
	return (cost);
}

/* Mark all the SINKs of this net as targets by setting their target flags  *
* to the number of times the net must connect to each SINK.  Note that     *
* this number can occasionally be greater than 1 -- think of connecting   *
* the same net to two inputs of an and-gate (and-gate inputs are logically *
* equivalent, so both will connect to the same SINK).                      */
void mark_ends(ClusterNetId net_id) {
	unsigned int ipin;
	int inode;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	for (ipin = 1; ipin < cluster_ctx.clb_nlist.net_pins(net_id).size(); ipin++) {
		inode = route_ctx.net_rr_terminals[net_id][ipin];
		route_ctx.rr_node_route_inf[inode].target_flag++;
	}
}

void mark_remaining_ends(const vector<int>& remaining_sinks) {
	// like mark_ends, but only performs it for the remaining sinks of a net
    auto& route_ctx = g_vpr_ctx.mutable_routing();
	for (int sink_node : remaining_sinks)
		++route_ctx.rr_node_route_inf[sink_node].target_flag;
}

void node_to_heap(int inode, float total_cost, int prev_node, int prev_edge,
		float backward_path_cost, float R_upstream) {

	/* Puts an rr_node on the heap, if the new cost given is lower than the     *
	 * current path_cost to this channel segment.  The index of its predecessor *
	 * is stored to make traceback easy.  The index of the edge used to get     *
	 * from its predecessor to it is also stored to make timing analysis, etc.  *
	 * easy.  The backward_path_cost and R_upstream values are used only by the *
	 * timing-driven router -- the breadth-first router ignores them.           */

    auto& route_ctx = g_vpr_ctx.routing();

	if (total_cost >= route_ctx.rr_node_route_inf[inode].path_cost)
		return;

	t_heap* hptr = alloc_heap_data();
	hptr->index = inode;
	hptr->cost = total_cost;
	hptr->u.prev_node = prev_node;
	hptr->prev_edge = prev_edge;
	hptr->backward_path_cost = backward_path_cost;
	hptr->R_upstream = R_upstream;
	add_to_heap(hptr);
}

void free_traceback(ClusterNetId net_id) {

	/* Puts the entire traceback (old routing) for this net on the free list *
	 * and sets the route_ctx.trace_head pointers etc. for the net to NULL.            */

	t_trace *tptr, *tempptr;

    auto& route_ctx = g_vpr_ctx.mutable_routing();

    if (route_ctx.trace_head.empty() && route_ctx.trace_tail.empty()) {
        return;
    }

	if(route_ctx.trace_head[net_id] == NULL) {
		return;
	}

	tptr = route_ctx.trace_head[net_id];

	while (tptr != NULL) {
		tempptr = tptr->next;
		free_trace_data(tptr);
		tptr = tempptr;
	}

	route_ctx.trace_head[net_id] = NULL;
	route_ctx.trace_tail[net_id] = NULL;
}

/* Allocates data structures into which the key routing data can be saved,   *
* allowing the routing to be recovered later (e.g. after a another routing  *
* is attempted).                                                            */
vtr::vector_map<ClusterNetId, t_trace *> alloc_saved_routing() {
	auto& cluster_ctx = g_vpr_ctx.clustering();
	vtr::vector_map<ClusterNetId, t_trace *> best_routing(cluster_ctx.clb_nlist.nets().size());

	return (best_routing);
}

/* TODO: super hacky, jluu comment, I need to rethink this whole function, without it, logically equivalent output pins incorrectly use more pins than needed.  I force that CLB output pin uses at most one output pin  */
static t_clb_opins_used alloc_and_load_clb_opins_used_locally(void) {

	/* Allocates and loads the data needed to make the router reserve some CLB  *
	 * output pins for connections made locally within a CLB (if the netlist    *
	 * specifies that this is necessary).                                       */

	t_clb_opins_used clb_opins_used_locally;
	int clb_pin, iclass, class_low, class_high;
	t_type_ptr type;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& device_ctx = g_vpr_ctx.device();

	clb_opins_used_locally.resize(cluster_ctx.clb_nlist.blocks().size());

	for (auto blk_id : cluster_ctx.clb_nlist.blocks()) {
		type = cluster_ctx.clb_nlist.block_type(blk_id);
		get_class_range_for_block(blk_id, &class_low, &class_high);
		clb_opins_used_locally[blk_id].resize(type->num_class);

        int pin_low = 0;
        int pin_high = 0;
        get_pin_range_for_block(blk_id, &pin_low, &pin_high);

		for (clb_pin = pin_low; clb_pin <= pin_high; clb_pin++) {
			// another hack to avoid I/Os, whole function needs a rethink
			if(type == device_ctx.IO_TYPE)
				continue;
			
			if ((cluster_ctx.clb_nlist.block_net(blk_id, clb_pin) != ClusterNetId::INVALID()
					&& cluster_ctx.clb_nlist.net_sinks(cluster_ctx.clb_nlist.block_net(blk_id, clb_pin)).size() == 0)
					|| cluster_ctx.clb_nlist.block_net(blk_id, clb_pin) == ClusterNetId::INVALID()) {
                
				iclass = type->pin_class[clb_pin];
				
				if(type->class_inf[iclass].type == DRIVER) {
					/* Check to make sure class is in same range as that assigned to block */
					VTR_ASSERT(iclass >= class_low && iclass <= class_high);
					clb_opins_used_locally[blk_id][iclass].emplace_back();
				}
			}
		}
	}

	return (clb_opins_used_locally);
}

/*the trace lists are only freed after use by the timing-driven placer */
	/*Do not  free them after use by the router, since stats, and draw  */
	/*routines use the trace values */
void free_trace_structs(void) {
    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

    if (route_ctx.trace_head.empty() && route_ctx.trace_tail.empty()) {
        return;
    }

	for (auto net_id : cluster_ctx.clb_nlist.nets()) {
		free_traceback(net_id);

		if (route_ctx.trace_head[net_id]) {
			free(route_ctx.trace_head[net_id]);
			free(route_ctx.trace_tail[net_id]);
		}
		route_ctx.trace_head[net_id] = NULL;
		route_ctx.trace_tail[net_id] = NULL;
	}
}

void free_route_structs() {

	/* Frees the temporary storage needed only during the routing.  The  *
	 * final routing result is not freed.                                */
    auto& route_ctx = g_vpr_ctx.mutable_routing();

	if(heap != NULL) {
        // coverity[offset_free : Intentional]
		free(heap + 1);
	}
	if(route_ctx.route_bb.size() != 0) {
		route_ctx.route_bb.clear();
	}

	heap = NULL; /* Defensive coding:  crash hard if I use these. */

	/*free the memory chunks that were used by heap and linked f pointer */
	free_chunk_memory(&heap_ch);
	free_chunk_memory(&linked_f_pointer_ch);
	heap_free_head = NULL;
	linked_f_pointer_free_head = NULL;
}

/* Frees the data structures needed to save a routing.                     */
void free_saved_routing(vtr::vector_map<ClusterNetId, t_trace *> &best_routing) {
	auto &cluster_ctx = g_vpr_ctx.clustering();
	for (auto net_id : cluster_ctx.clb_nlist.nets()) {
		if (best_routing[net_id] != NULL) {
			free(best_routing[net_id]);
			best_routing[net_id] = NULL;
		}
	}
}

void alloc_and_load_rr_node_route_structs(void) {

	/* Allocates some extra information about each rr_node that is used only   *
	 * during routing.                                                         */

    auto& route_ctx = g_vpr_ctx.mutable_routing();
    auto& device_ctx = g_vpr_ctx.device();

    route_ctx.rr_node_route_inf.resize(device_ctx.num_rr_nodes);
    reset_rr_node_route_structs();
}

void reset_rr_node_route_structs(void) {

	/* Resets some extra information about each rr_node that is used only   *
	 * during routing.                                                         */

    auto& route_ctx = g_vpr_ctx.mutable_routing();
    auto& device_ctx = g_vpr_ctx.device();

	VTR_ASSERT(route_ctx.rr_node_route_inf.size() == size_t(device_ctx.num_rr_nodes));

	for (int inode = 0; inode < device_ctx.num_rr_nodes; inode++) {
		route_ctx.rr_node_route_inf[inode].prev_node = NO_PREVIOUS;
		route_ctx.rr_node_route_inf[inode].prev_edge = NO_PREVIOUS;
		route_ctx.rr_node_route_inf[inode].pres_cost = 1.0;
		route_ctx.rr_node_route_inf[inode].acc_cost = 1.0;
		route_ctx.rr_node_route_inf[inode].path_cost = HUGE_POSITIVE_FLOAT;
		route_ctx.rr_node_route_inf[inode].target_flag = 0;
		route_ctx.rr_node_route_inf[inode].set_occ(0);
	}
}


/* Allocates and loads the route_ctx.net_rr_terminals data structure. For each net it stores the rr_node   *
* index of the SOURCE of the net and all the SINKs of the net [clb_nlist.nets()][clb_nlist.net_pins()].    * 
* Entry [inet][pnum] stores the rr index corresponding to the SOURCE (opin) or SINK (ipin) of the pin.     */
static vtr::vector_map<ClusterNetId, std::vector<int>> load_net_rr_terminals(const t_rr_node_indices& L_rr_node_indices) {
    vtr::vector_map<ClusterNetId, std::vector<int>> net_rr_terminals;

    int inode, i, j, node_block_pin, iclass;
    t_type_ptr type;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& place_ctx = g_vpr_ctx.placement();

    auto nets = cluster_ctx.clb_nlist.nets();
    net_rr_terminals.resize(nets.size());

	for (auto net_id : cluster_ctx.clb_nlist.nets()) {
        auto net_pins = cluster_ctx.clb_nlist.net_pins(net_id);
        net_rr_terminals[net_id].resize(net_pins.size());

		int pin_count = 0;
		for (auto pin_id : cluster_ctx.clb_nlist.net_pins(net_id)) {
			auto block_id = cluster_ctx.clb_nlist.pin_block(pin_id);
			i = place_ctx.block_locs[block_id].x;
			j = place_ctx.block_locs[block_id].y;
            type = cluster_ctx.clb_nlist.block_type(block_id);

            /* In the routing graph, each (x, y) location has unique pins on it
             * so when there is capacity, blocks are packed and their pin numbers
             * are offset to get their actual rr_node */
            node_block_pin = cluster_ctx.clb_nlist.pin_physical_index(pin_id);

            iclass = type->pin_class[node_block_pin];

            inode = get_rr_node_index(L_rr_node_indices, i, j, (pin_count == 0 ? SOURCE : SINK), /* First pin is driver */
                    iclass);
            net_rr_terminals[net_id][pin_count] = inode;
			pin_count++;
        }
    }

    return net_rr_terminals;
}

/* Saves the rr_node corresponding to each SOURCE and SINK in each CLB      *
* in the FPGA.  Currently only the SOURCE rr_node values are used, and     *
* they are used only to reserve pins for locally used OPINs in the router. *
* [0..cluster_ctx.clb_nlist.blocks().size()-1][0..num_class-1].            *
* The values for blocks that are padsare NOT valid.                        */
static vtr::vector_map<ClusterBlockId, std::vector<int>> load_rr_clb_sources(const t_rr_node_indices& L_rr_node_indices) {
    vtr::vector_map<ClusterBlockId, std::vector<int>> rr_blk_source;

	int i, j, iclass, inode;
    int class_low, class_high;
    t_rr_type rr_type;
    t_type_ptr type;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& place_ctx = g_vpr_ctx.placement();

    rr_blk_source.resize(cluster_ctx.clb_nlist.blocks().size());

	for (auto blk_id : cluster_ctx.clb_nlist.blocks()) {
        type = cluster_ctx.clb_nlist.block_type(blk_id);
        get_class_range_for_block(blk_id, &class_low, &class_high);
		rr_blk_source[blk_id].resize(type->num_class);
        for (iclass = 0; iclass < type->num_class; iclass++) {
            if (iclass >= class_low && iclass <= class_high) {
                i = place_ctx.block_locs[blk_id].x;
                j = place_ctx.block_locs[blk_id].y;

                if (type->class_inf[iclass].type == DRIVER)
                    rr_type = SOURCE;
                else
                    rr_type = SINK;

                inode = get_rr_node_index(L_rr_node_indices, i, j, rr_type, iclass);
                rr_blk_source[blk_id][iclass] = inode;
            } else {
                rr_blk_source[blk_id][iclass] = OPEN;
            }
        }
    }

    return rr_blk_source;
}


static vtr::vector_map<ClusterNetId, t_bb> load_route_bb(int bb_factor) {

	/* This routine loads the bounding box arrays used to limit the space  *
	 * searched by the maze router when routing each net.  The search is   *
	 * limited to channels contained with the net bounding box expanded    *
	 * by bb_factor channels on each side.  For example, if bb_factor is   *
	 * 0, the maze router must route each net within its bounding box.     *
	 * If bb_factor = max(device_ctx.grid.width()-1, device_cts.grid.height() - 1),
     * the maze router will search every channel in     *
	 * the FPGA if necessary.  The bounding boxes returned by this routine *
	 * are different from the ones used by the placer in that they are     * 
	 * clipped to lie within (0,0) and (device_ctx.grid.width()-1,device_ctx.grid.height()-1) rather than (1,1) and   *
	 * (device_ctx.grid.width()-1,device_ctx.grid.height()-1).                                                            */
    vtr::vector_map<ClusterNetId, t_bb> route_bb;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& device_ctx = g_vpr_ctx.device();
    auto& route_ctx = g_vpr_ctx.routing();

    auto nets = cluster_ctx.clb_nlist.nets();
    route_bb.resize(nets.size());
	for (auto net_id : nets) {
        int driver_rr = route_ctx.net_rr_terminals[net_id][0];
        const t_rr_node& source_node = device_ctx.rr_nodes[driver_rr];
        VTR_ASSERT(source_node.type() == SOURCE);

        VTR_ASSERT(source_node.xlow() <= source_node.xhigh());
        VTR_ASSERT(source_node.ylow() <= source_node.yhigh());

		int xmin = source_node.xlow();
		int ymin = source_node.ylow();
		int xmax = source_node.xhigh();
		int ymax = source_node.yhigh();

        auto net_sinks = cluster_ctx.clb_nlist.net_sinks(net_id);
		for (size_t ipin = 1; ipin < net_sinks.size() + 1; ++ipin) { //Start at 1 since looping through sinks
            int sink_rr = route_ctx.net_rr_terminals[net_id][ipin];
            const t_rr_node& sink_node = device_ctx.rr_nodes[sink_rr];
            VTR_ASSERT(sink_node.type() == SINK);

            VTR_ASSERT(sink_node.xlow() <= sink_node.xhigh());
            VTR_ASSERT(sink_node.ylow() <= sink_node.yhigh());

            xmin = std::min<int>(xmin, sink_node.xlow());
            xmax = std::max<int>(xmax, sink_node.xhigh());
            ymin = std::min<int>(ymin, sink_node.ylow());
            ymax = std::max<int>(ymax, sink_node.yhigh());
		}

		/* Want the channels on all 4 sides to be usuable, even if bb_factor = 0. */
		xmin -= 1;
		ymin -= 1;

		/* Expand the net bounding box by bb_factor, then clip to the physical *
		 * chip area.                                                          */

		route_bb[net_id].xmin = max<int>(xmin - bb_factor, 0);
		route_bb[net_id].xmax = min<int>(xmax + bb_factor, device_ctx.grid.width() - 1);
		route_bb[net_id].ymin = max<int>(ymin - bb_factor, 0);
		route_bb[net_id].ymax = min<int>(ymax + bb_factor, device_ctx.grid.height() - 1);
	}
    return route_bb;
}

void add_to_mod_list(float *fptr) {

	/* This routine adds the floating point pointer (fptr) into a  *
	 * linked list that indicates all the pathcosts that have been *
	 * modified thus far.                                          */

	t_linked_f_pointer *mod_ptr;

	mod_ptr = alloc_linked_f_pointer();

	/* Add this element to the start of the modified list. */

	mod_ptr->next = rr_modified_head;
	mod_ptr->fptr = fptr;
	rr_modified_head = mod_ptr;
}
namespace heap_ {
	size_t parent(size_t i);
	size_t left(size_t i);
	size_t right(size_t i);
	size_t size();
	void expand_heap_if_full();

	size_t parent(size_t i) {return i >> 1;}
	// child indices of a heap
	size_t left(size_t i) {return i << 1;}
	size_t right(size_t i) {return (i << 1) + 1;}
	size_t size() {return static_cast<size_t>(heap_tail - 1);}	// heap[0] is not valid element

	// make a heap rooted at index i by **sifting down** in O(lgn) time
	void sift_down(size_t hole) {
		t_heap* head {heap[hole]};
		size_t child {left(hole)};
		while ((int)child < heap_tail) {
			if ((int)child + 1 < heap_tail && heap[child + 1]->cost < heap[child]->cost)
				++child;
			if (heap[child]->cost < head->cost) {
				heap[hole] = heap[child];
				hole = child;
				child = left(child);
			}
			else break;
		}
		heap[hole] = head;
	}


	// runs in O(n) time by sifting down; the least work is done on the most elements: 1 swap for bottom layer, 2 swap for 2nd, ... lgn swap for top
	// 1*(n/2) + 2*(n/4) + 3*(n/8) + ... + lgn*1 = 2n (sum of i/2^i)
	void build_heap() {
		// second half of heap are leaves
		for (size_t i = heap_tail >> 1; i != 0; --i)
			sift_down(i);
	}


	// O(lgn) sifting up to maintain heap property after insertion (should sift down when building heap)
	void sift_up(size_t leaf, t_heap* const node) {
		while ((leaf > 1) && (node->cost < heap[parent(leaf)]->cost)) {
			// sift hole up
			heap[leaf] = heap[parent(leaf)];
			leaf = parent(leaf);
		}
		heap[leaf] = node;
	}


	void expand_heap_if_full() {
		if (heap_tail > heap_size) { /* Heap is full */
			heap_size *= 2;
			heap = (t_heap **) vtr::realloc((void *) (heap + 1),
					heap_size * sizeof(t_heap *));
			heap--; /* heap goes from [1..heap_size] */
		}		
	}

	// adds an element to the back of heap and expand if necessary, but does not maintain heap property
	void push_back(t_heap* const hptr) {
		expand_heap_if_full();
		heap[heap_tail] = hptr;
		++heap_tail;
	}


	void push_back_node(int inode, float total_cost, int prev_node, int prev_edge,
		float backward_path_cost, float R_upstream) {

		/* Puts an rr_node on the heap with the same condition as node_to_heap,
		   but do not fix heap property yet as that is more efficiently done from
		   bottom up with build_heap    */
	
        auto& route_ctx = g_vpr_ctx.routing();
		if (total_cost >= route_ctx.rr_node_route_inf[inode].path_cost)
			return;

		t_heap* hptr = alloc_heap_data();
		hptr->index = inode;
		hptr->cost = total_cost;
		hptr->u.prev_node = prev_node;
		hptr->prev_edge = prev_edge;
		hptr->backward_path_cost = backward_path_cost;
		hptr->R_upstream = R_upstream;
		push_back(hptr);
	}

	bool is_valid() {
		for (size_t i = 1; (int)i <= heap_tail >> 1; ++i) {
			if ((int)left(i) < heap_tail && heap[left(i)]->cost < heap[i]->cost) return false;
			if ((int)right(i) < heap_tail && heap[right(i)]->cost < heap[i]->cost) return false;
		}
		return true;
	}
	// extract every element and print it
	void pop_heap() {
		while (!is_empty_heap()) vtr::printf_info("%e ", get_heap_head()->cost);
		vtr::printf_info("\n");
	}
	// print every element; not necessarily in order for minheap
	void print_heap() {
		for (int i = 1; i < heap_tail >> 1; ++i) vtr::printf_info("(%e %e %e) ", heap[i]->cost, heap[left(i)]->cost, heap[right(i)]->cost);
		vtr::printf_info("\n");
	}
	// verify correctness of extract top by making a copy, sorting it, and iterating it at the same time as extraction
	void verify_extract_top() {
		constexpr float float_epsilon = 1e-20;
		std::cout << "copying heap\n";
		std::vector<t_heap*> heap_copy {heap + 1, heap + heap_tail};
		// sort based on cost with cheapest first
		VTR_ASSERT(heap_copy.size() == size());
		std::sort(begin(heap_copy), end(heap_copy), 
			[](const t_heap* a, const t_heap* b){
			return a->cost < b->cost;
		});
		std::cout << "starting to compare top elements\n";
		size_t i = 0;
		while (!is_empty_heap()) {
			while (heap_copy[i]->index == OPEN) ++i;	// skip the ones that won't be extracted
			auto top = get_heap_head();
			if (abs(top->cost - heap_copy[i]->cost) > float_epsilon)	
				std::cout << "mismatch with sorted " << top << '(' << top->cost << ") " << heap_copy[i] << '(' << heap_copy[i]->cost << ")\n";
			++i;
		}
		if (i != heap_copy.size()) std::cout << "did not finish extracting: " << i << " vs " << heap_copy.size() << std::endl;
		else std::cout << "extract top working as intended\n";
	}
}
// adds to heap and maintains heap quality
static void add_to_heap(t_heap *hptr) {
	heap_::expand_heap_if_full();
	// start with undefined hole
	++heap_tail;	
	heap_::sift_up(heap_tail - 1, hptr);
}

/*WMF: peeking accessor :) */
bool is_empty_heap(void) {
	return (bool)(heap_tail == 1);
}

t_heap *
get_heap_head(void) {

	/* Returns a pointer to the smallest element on the heap, or NULL if the     *
	 * heap is empty.  Invalid (index == OPEN) entries on the heap are never     *
	 * returned -- they are just skipped over.                                   */

	t_heap *cheapest;
	size_t hole, child;

	do {
		if (heap_tail == 1) { /* Empty heap. */
			vtr::printf_warning(__FILE__, __LINE__, "Empty heap occurred in get_heap_head.\n");
			vtr::printf_warning(__FILE__, __LINE__, "Some blocks are impossible to connect in this architecture.\n");
			return (NULL);
		}

		cheapest = heap[1]; 

		hole = 1;
		child = 2;
		--heap_tail;
		while ((int)child < heap_tail) {
			if (heap[child + 1]->cost < heap[child]->cost)
				++child;	// become right child
			heap[hole] = heap[child];
			hole = child;
			child = heap_::left(child);
		}
		heap_::sift_up(hole, heap[heap_tail]);

	} while (cheapest->index == OPEN); /* Get another one if invalid entry. */

	return (cheapest);
}

void empty_heap(void) {

	for (int i = 1; i < heap_tail; i++)
		free_heap_data(heap[i]);

	heap_tail = 1;
}

static t_heap *
alloc_heap_data(void) {

	t_heap *temp_ptr;

	if (heap_free_head == NULL) { /* No elements on the free list */
		heap_free_head = (t_heap *) vtr::chunk_malloc(sizeof(t_heap),&heap_ch);
		heap_free_head->u.next = NULL;
	}

	temp_ptr = heap_free_head;
	heap_free_head = heap_free_head->u.next;
	num_heap_allocated++;
	return (temp_ptr);
}

void free_heap_data(t_heap *hptr) {

	hptr->u.next = heap_free_head;
	heap_free_head = hptr;
	num_heap_allocated--;
}

void invalidate_heap_entries(int sink_node, int ipin_node) {

	/* Marks all the heap entries consisting of sink_node, where it was reached *
	 * via ipin_node, as invalid (OPEN).  Used only by the breadth_first router *
	 * and even then only in rare circumstances.                                */

	for (int i = 1; i < heap_tail; i++) {
		if (heap[i]->index == sink_node && heap[i]->u.prev_node == ipin_node)
			heap[i]->index = OPEN; /* Invalid. */
	}
}

t_trace *
alloc_trace_data(void) {

	t_trace *temp_ptr;

	if (trace_free_head == NULL) { /* No elements on the free list */
		trace_free_head = (t_trace *) vtr::chunk_malloc(sizeof(t_trace),&trace_ch);
		trace_free_head->next = NULL;
	}
	temp_ptr = trace_free_head;
	trace_free_head = trace_free_head->next;
	num_trace_allocated++;
	return (temp_ptr);
}

void free_trace_data(t_trace *tptr) {

	/* Puts the traceback structure pointed to by tptr on the free list. */

	tptr->next = trace_free_head;
	trace_free_head = tptr;
	num_trace_allocated--;
}

static t_linked_f_pointer *
alloc_linked_f_pointer(void) {

	/* This routine returns a linked list element with a float pointer as *
	 * the node data.                                                     */

	/*int i;*/
	t_linked_f_pointer *temp_ptr;

	if (linked_f_pointer_free_head == NULL) {
		/* No elements on the free list */	
	linked_f_pointer_free_head = (t_linked_f_pointer *) vtr::chunk_malloc(sizeof(t_linked_f_pointer),&linked_f_pointer_ch);
	linked_f_pointer_free_head->next = NULL;
	}

	temp_ptr = linked_f_pointer_free_head;
	linked_f_pointer_free_head = linked_f_pointer_free_head->next;

	num_linked_f_pointer_allocated++;

	return (temp_ptr);
}

/* Prints out the routing to file route_file.  */
void print_route(const char* placement_file, const char* route_file) {
	int inode, ilow, jlow, iclass;
	t_rr_type rr_type;
	t_trace *tptr;
	FILE *fp;

	fp = fopen(route_file, "w");

    auto& place_ctx = g_vpr_ctx.placement();
    auto& device_ctx = g_vpr_ctx.device();
    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();

    fprintf(fp, "Placement_File: %s Placement_ID: %s\n", placement_file, place_ctx.placement_id.c_str());

	fprintf(fp, "Array size: %zu x %zu logic blocks.\n", device_ctx.grid.width(), device_ctx.grid.height());
	fprintf(fp, "\nRouting:");
	for (auto net_id : cluster_ctx.clb_nlist.nets()) {
		if (!cluster_ctx.clb_nlist.net_is_global(net_id)) {
			fprintf(fp, "\n\nNet %zu (%s)\n\n", size_t(net_id), cluster_ctx.clb_nlist.net_name(net_id).c_str());
			if (cluster_ctx.clb_nlist.net_sinks(net_id).size() == false) {
				fprintf(fp, "\n\nUsed in local cluster only, reserved one CLB pin\n\n");
			} else {
				tptr = route_ctx.trace_head[net_id];

				while (tptr != NULL) {
					inode = tptr->index;
					rr_type = device_ctx.rr_nodes[inode].type();
					ilow = device_ctx.rr_nodes[inode].xlow();
					jlow = device_ctx.rr_nodes[inode].ylow();

					fprintf(fp, "Node:\t%d\t%6s (%d,%d) ", inode, 
							device_ctx.rr_nodes[inode].type_string(), ilow, jlow);

					if ((ilow != device_ctx.rr_nodes[inode].xhigh())
							|| (jlow != device_ctx.rr_nodes[inode].yhigh()))
						fprintf(fp, "to (%d,%d) ", device_ctx.rr_nodes[inode].xhigh(),
								device_ctx.rr_nodes[inode].yhigh());

					switch (rr_type) {

					case IPIN:
					case OPIN:
						if (device_ctx.grid[ilow][jlow].type == device_ctx.IO_TYPE) {
							fprintf(fp, " Pad: ");
						} else { /* IO Pad. */
							fprintf(fp, " Pin: ");
						}
						break;

					case CHANX:
					case CHANY:
						fprintf(fp, " Track: ");
						break;

					case SOURCE:
					case SINK:
						if (device_ctx.grid[ilow][jlow].type == device_ctx.IO_TYPE) {
							fprintf(fp, " Pad: ");
						} else { /* IO Pad. */
							fprintf(fp, " Class: ");
						}
						break;

					default:
						vpr_throw(VPR_ERROR_ROUTE, __FILE__, __LINE__, 
								  "in print_route: Unexpected traceback element type: %d (%s).\n", 
								  rr_type, device_ctx.rr_nodes[inode].type_string());
						break;
					}

					fprintf(fp, "%d  ", device_ctx.rr_nodes[inode].ptc_num());

					if (device_ctx.grid[ilow][jlow].type != device_ctx.IO_TYPE && (rr_type == IPIN || rr_type == OPIN)) {
						int pin_num = device_ctx.rr_nodes[inode].ptc_num();
						int xoffset = device_ctx.grid[ilow][jlow].width_offset;
						int yoffset = device_ctx.grid[ilow][jlow].height_offset;
						ClusterBlockId iblock = place_ctx.grid_blocks[ilow - xoffset][jlow - yoffset].blocks[0];
                        VTR_ASSERT(iblock);
						t_pb_graph_pin *pb_pin = get_pb_graph_node_pin_from_block_pin(iblock, pin_num);
						t_pb_type *pb_type = pb_pin->parent_node->pb_type;
						fprintf(fp, " %s.%s[%d] ", pb_type->name, pb_pin->port->name, pb_pin->pin_number);
					}

					/* Uncomment line below if you're debugging and want to see the switch types *
					 * used in the routing.                                                      */
					fprintf (fp, "Switch: %d", tptr->iswitch);    

					fprintf(fp, "\n");

					tptr = tptr->next;
				}
			}
		}

		else { /* Global net.  Never routed. */
			fprintf(fp, "\n\nNet %zu (%s): global net connecting:\n\n", size_t(net_id),
					cluster_ctx.clb_nlist.net_name(net_id).c_str());

			for (auto pin_id : cluster_ctx.clb_nlist.net_pins(net_id)) {
				ClusterBlockId block_id = cluster_ctx.clb_nlist.pin_block(pin_id);
				int pin_index = cluster_ctx.clb_nlist.pin_physical_index(pin_id);
				iclass = cluster_ctx.clb_nlist.block_type(block_id)->pin_class[pin_index];

				fprintf(fp, "Block %s (#%zu) at (%d,%d), Pin class %d.\n",
					cluster_ctx.clb_nlist.block_name(block_id).c_str(), size_t(block_id),
					place_ctx.block_locs[block_id].x, 
					place_ctx.block_locs[block_id].y,
					iclass);
			}
		}
	}

	fclose(fp);

	if (getEchoEnabled() && isEchoFileEnabled(E_ECHO_MEM)) {
		fp = vtr::fopen(getEchoFileName(E_ECHO_MEM), "w");
		fprintf(fp, "\nNum_heap_allocated: %d   Num_trace_allocated: %d\n",
				num_heap_allocated, num_trace_allocated);
		fprintf(fp, "Num_linked_f_pointer_allocated: %d\n",
				num_linked_f_pointer_allocated);
		fclose(fp);
	}

    //Save the digest of the route file
    route_ctx.routing_id = vtr::secure_digest_file(route_file);
}

/* TODO: jluu: I now always enforce logically equivalent outputs to use at most one output pin, should rethink how to do this */
void reserve_locally_used_opins(float pres_fac, float acc_fac, bool rip_up_local_opins) {

	/* In the past, this function implicitly allowed LUT duplication when there are free LUTs. 
	 This was especially important for logical equivalence; however, now that we have a very general logic cluster,
	 it does not make sense to allow LUT duplication implicitly. We'll need to look into how we want to handle this case
	 */

	int num_local_opin, inode, from_node, iconn, num_edges, to_node;
	int iclass, ipin;
	float cost;
	t_heap *heap_head_ptr;
	t_type_ptr type;

    auto& cluster_ctx = g_vpr_ctx.clustering();
    auto& route_ctx = g_vpr_ctx.mutable_routing();
    auto& device_ctx = g_vpr_ctx.device();

	if (rip_up_local_opins) {
		for (auto blk_id : cluster_ctx.clb_nlist.blocks()) {
			type = cluster_ctx.clb_nlist.block_type(blk_id);
			for (iclass = 0; iclass < type->num_class; iclass++) {
				num_local_opin = route_ctx.clb_opins_used_locally[blk_id][iclass].size();
				/* Always 0 for pads and for RECEIVER (IPIN) classes */
				for (ipin = 0; ipin < num_local_opin; ipin++) {
					inode = route_ctx.clb_opins_used_locally[blk_id][iclass][ipin];
					adjust_one_rr_occ_and_apcost(inode, -1, pres_fac, acc_fac);
				}
			}
		}
	}

	for (auto blk_id : cluster_ctx.clb_nlist.blocks()) {
		type = cluster_ctx.clb_nlist.block_type(blk_id);
		for (iclass = 0; iclass < type->num_class; iclass++) {
			num_local_opin = route_ctx.clb_opins_used_locally[blk_id][iclass].size();
			/* Always 0 for pads and for RECEIVER (IPIN) classes */

			if (num_local_opin != 0) { /* Have to reserve (use) some OPINs */
				from_node = route_ctx.rr_blk_source[blk_id][iclass];
				num_edges = device_ctx.rr_nodes[from_node].num_edges();
				for (iconn = 0; iconn < num_edges; iconn++) {
					to_node = device_ctx.rr_nodes[from_node].edge_sink_node(iconn);
					cost = get_rr_cong_cost(to_node);
					node_to_heap(to_node, cost, OPEN, OPEN, 0., 0.);
                                }

				for (ipin = 0; ipin < num_local_opin; ipin++) {
					heap_head_ptr = get_heap_head();
					inode = heap_head_ptr->index;
					adjust_one_rr_occ_and_apcost(inode, 1, pres_fac, acc_fac);
					route_ctx.clb_opins_used_locally[blk_id][iclass][ipin] = inode;
					free_heap_data(heap_head_ptr);
				}

				empty_heap();
			}
		}
	}
}

static void adjust_one_rr_occ_and_apcost(int inode, int add_or_sub,
		float pres_fac, float acc_fac) {

	/* Increments or decrements (depending on add_or_sub) the occupancy of    *
	 * one rr_node, and adjusts the present cost of that node appropriately.  */

	int occ, capacity;

    auto& route_ctx = g_vpr_ctx.mutable_routing();
    auto& device_ctx = g_vpr_ctx.device();

	occ = route_ctx.rr_node_route_inf[inode].occ() + add_or_sub;
	capacity = device_ctx.rr_nodes[inode].capacity();
	route_ctx.rr_node_route_inf[inode].set_occ(occ);

	if (occ < capacity) {
		route_ctx.rr_node_route_inf[inode].pres_cost = 1.0;
	} else {
		route_ctx.rr_node_route_inf[inode].pres_cost = 1.0 + (occ + 1 - capacity) * pres_fac;
		if (add_or_sub == 1) {
			route_ctx.rr_node_route_inf[inode].acc_cost += (occ - capacity) * acc_fac;
		}
	}
}

void free_chunk_memory_trace(void) {
	if (trace_ch.chunk_ptr_head != NULL) {
		free_chunk_memory(&trace_ch);
	}
}


// connection based overhaul (more specificity than nets)
// utility and debugging functions -----------------------
void print_traceback(ClusterNetId net_id) {
	// linearly print linked list
    auto& route_ctx = g_vpr_ctx.routing();
    auto& device_ctx = g_vpr_ctx.device();

	vtr::printf_info("traceback %zu: ", size_t(net_id));
	t_trace* head = route_ctx.trace_head[net_id];
	while (head) {
		int inode {head->index};
		if (device_ctx.rr_nodes[inode].type() == SINK) 
			vtr::printf_info("%d(sink)(%d)->",inode, route_ctx.rr_node_route_inf[inode].occ());
		else 
			vtr::printf_info("%d(%d)->",inode, route_ctx.rr_node_route_inf[inode].occ());
		head = head->next;
	}
	vtr::printf_info("\n");
}