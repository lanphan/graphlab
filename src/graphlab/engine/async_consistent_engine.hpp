/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */




#ifndef GRAPHLAB_ASYNC_CONSISTENT_ENGINE
#define GRAPHLAB_ASYNC_CONSISTENT_ENGINE

#include <deque>
#include <boost/bind.hpp>

#include <graphlab/scheduler/ischeduler.hpp>
#include <graphlab/scheduler/scheduler_factory.hpp>
#include <graphlab/vertex_program/ivertex_program.hpp>
#include <graphlab/vertex_program/icontext.hpp>
#include <graphlab/vertex_program/context.hpp>
#include <graphlab/engine/iengine.hpp>
#include <graphlab/engine/execution_status.hpp>
#include <graphlab/options/graphlab_options.hpp>
#include <graphlab/rpc/dc_dist_object.hpp>
#include <graphlab/rpc/distributed_chandy_misra.hpp>

#include <graphlab/util/tracepoint.hpp>
#include <graphlab/util/memory_info.hpp>
#include <graphlab/rpc/distributed_event_log.hpp>
#include <graphlab/rpc/async_consensus.hpp>

#include <graphlab/macros_def.hpp>

namespace graphlab {
  
  

  template<typename VertexProgram>
  class async_consistent_engine: public iengine<VertexProgram> {
      
  public:
    // Include parent types
    typedef VertexProgram vertex_program_type;
    typedef typename VertexProgram::gather_type gather_type;
    typedef conditional_addition_wrapper<gather_type> conditional_gather_type;
    
    typedef typename VertexProgram::message_type message_type;
    typedef typename VertexProgram::vertex_data_type vertex_data_type;
    typedef typename VertexProgram::edge_data_type edge_data_type;

    typedef distributed_graph<vertex_data_type, edge_data_type> graph_type;
    typedef typename graph_type::vertex_type          vertex_type;
    typedef typename graph_type::edge_type            edge_type;

    typedef typename graph_type::local_vertex_type    local_vertex_type;
    typedef typename graph_type::local_edge_type      local_edge_type;
    typedef typename graph_type::lvid_type            lvid_type;

    typedef ischeduler<message_type> ischeduler_type;

    typedef context<async_consistent_engine> context_type;
    typedef async_consistent_engine<VertexProgram> engine_type;
    
    enum vertex_execution_state {
      NONE = 0,
      LOCKING,     // state on owner
      GATHERING,   // state on owner
      APPLYING,    // state on owner
      SCATTERING,  // state on owner
      MIRROR_GATHERING, // state on mirror
      MIRROR_SCATTERING, // state on mirror
      MIRROR_SCATTERING_AND_NEXT_LOCKING, // state on mirror
    }; // end of vertex execution state


    struct vertex_state {
      vertex_program_type vertex_program;
      message_type current_message;
      conditional_gather_type combined_gather;
      uint32_t apply_count_down;    // used to count down the gathers
      bool hasnext;
      simple_spinlock slock;
      vertex_execution_state state; // current state of the vertex 
      vertex_state(): apply_count_down(0), hasnext(false), state(NONE) { }
      std::ostream& operator<<(std::ostream& os) const {
        switch(state) {
        case NONE: { os << "NONE"; break; }
        case GATHERING: { os << "GATHERING: " << apply_count_down; break; }
        case APPLYING: { os << "APPLYING"; break; }
        case SCATTERING: { os << "SCATTERING"; break; }
        case MIRROR_GATHERING: { os << "MIRROR_GATHERING"; break; }
        case MIRROR_SCATTERING: { os << "MIRROR_SCATTERING"; break; }
        case MIRROR_SCATTERING_AND_NEXT_LOCKING: { os << "MIRROR_SCATTERING_AND_NEXT_LOCKING"; break; }
        }
        return os;
      }
      
      void lock() {
        slock.lock();
      }
      void unlock() {
        slock.unlock();
      }
    }; // end of vertex_state
    
    
    struct thread_local_data {
      mutex lock;
      size_t npending;
      std::deque<vertex_id_type> pending_vertices;
      thread_local_data() : npending(0) { }       
      void add_task(vertex_id_type v) {
        lock.lock();
        ++npending;
        pending_vertices.push_back(v);
        lock.unlock();
      }
      void add_task_priority(vertex_id_type v) {
        lock.lock();
        ++npending;
        pending_vertices.push_front(v);
        lock.unlock();
      }
      bool get_task(std::deque<vertex_id_type> &v) {
        v = std::deque<vertex_id_type>();
        lock.lock();
        if (npending == 0) { lock.unlock(); return false; }
        npending = 0;
        v.swap(pending_vertices);
        lock.unlock();
        return true;
      }
    }; // end of thread local data

  private:
    dc_dist_object<async_consistent_engine<VertexProgram> > rmi;

    graph_type& graph;

    distributed_chandy_misra<graph_type>* cmlocks;

    thread_group thrgroup;
    
    //! The scheduler
    ischeduler_type* scheduler_ptr;
    
    std::vector<vertex_state> vstate;
    
    size_t ncpus;
    bool started;
    async_consensus* consensus;
    atomic<size_t> threads_alive;
    std::vector<thread_local_data> thrlocal;
    
    atomic<uint64_t> joined_messages;
    atomic<uint64_t> blocked_issues; // issued messages which either
                                     // 1: cannot start and have to be 
                                     //    reinjected into the 
                                     //    scheduler.
                                     // 2: issued but is combined into a
                                     //    message which is currently locking
    atomic<uint64_t> issued_messages;
    atomic<uint64_t> programs_executed;
    float max_clean_fraction; // engine option
    size_t max_clean_forks; // set at start(). #edges * clean_fraction
    size_t timed_termination; // engine option
    float engine_start_time;
    bool timeout;
    
    graphlab_options opts_copy; // local copy of options to pass to 
                                // scheduler construction
    
    execution_status::status_enum termination_reason; 

    PERMANENT_DECLARE_DIST_EVENT_LOG(eventlog);
    DECLARE_TRACER(disteng_eval_sched_task);
    DECLARE_TRACER(disteng_chandy_misra);
    DECLARE_TRACER(disteng_init_gathering); 
    DECLARE_TRACER(disteng_init_scattering);
    DECLARE_TRACER(disteng_evalfac);
    DECLARE_TRACER(disteng_internal_task_queue);

    
    bool try_to_quit(size_t threadid,
                     bool& has_internal_task,
                     std::deque<lvid_type>& internal_lvid,
                     bool& has_sched_msg,
                     lvid_type& sched_lvid,
                     message_type &msg) {
      static size_t ctr = 0;
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, NO_WORK_EVENT, 1);
      if (lowres_time_seconds() - engine_start_time > timed_termination) {
        termination_reason = execution_status::TIMEOUT;
        timeout = true;
      }
      if (!timeout &&
          issued_messages.value != programs_executed.value + blocked_issues.value) {
        ++ctr;
        if (ctr % 10 == 0) usleep(1);
        return false;
      }
      logstream(LOG_DEBUG) << rmi.procid() << "-" << threadid << ": " << "Termination Attempt " 
                           << programs_executed.value << "/" << issued_messages.value << std::endl;
      has_internal_task = false;
      has_sched_msg = false;
      threads_alive.dec();
      consensus->begin_done_critical_section(threadid);
      
      BEGIN_TRACEPOINT(disteng_internal_task_queue);
      if (thrlocal[threadid].get_task(internal_lvid)) {
        logstream(LOG_DEBUG) << rmi.procid() << "-" << threadid <<  ": "
                             << "\tCancelled by Internal Task"  << std::endl;
        has_internal_task = true;
        consensus->cancel_critical_section(threadid);
        threads_alive.inc();
        END_TRACEPOINT(disteng_internal_task_queue);
        return false;
      }
      END_TRACEPOINT(disteng_internal_task_queue);
      
      sched_status::status_enum stat = 
        scheduler_ptr->get_next(threadid, sched_lvid, msg);
      if (stat == sched_status::EMPTY) {
        logstream(LOG_DEBUG) << rmi.procid() << "-" << threadid <<  ": " 
                             << "\tTermination Double Checked" << std::endl;
        bool ret = consensus->end_done_critical_section(threadid);
        threads_alive.inc();
        if (ret == false) {
          logstream(LOG_DEBUG) << rmi.procid() << "-" << threadid <<  ": " 
                             << "\tCancelled" << std::endl;
        }
        return ret;
      } else {
        logstream(LOG_DEBUG) << rmi.procid() << "-" << threadid <<  ": " 
                             << "\tCancelled by Scheduler Task" << std::endl;
        consensus->cancel_critical_section(threadid);
        has_sched_msg = true;
        threads_alive.inc();
        return false;
      }
    } // end of try to quit

    inline void ASSERT_I_AM_OWNER(const lvid_type lvid) const {
      ASSERT_EQ(graph.l_get_vertex_record(lvid).owner, rmi.procid());
    }
    inline void ASSERT_I_AM_NOT_OWNER(const lvid_type lvid) const {
      ASSERT_NE(graph.l_get_vertex_record(lvid).owner, rmi.procid());
    }

    enum {
      SCHEDULE_EVENT = 0,
      UPDATE_EVENT = 1,
      WORK_ISSUED_EVENT = 2,
      INTERNAL_TASK_EVENT = 3,
      NO_WORK_EVENT = 4,
      SCHEDULE_FROM_REMOTE_EVENT = 5,
      USER_OP_EVENT = 6,
      ENGINE_START_EVENT = 7,
      ENGINE_STOP_EVENT = 8
    };



    void schedule_local_next(vertex_id_type local_vid) {
      if (timeout) return;
      if (started) {
        scheduler_ptr->schedule_from_execution_thread(thread::thread_id(),
                                                      local_vid);
      }
      else {
        scheduler_ptr->schedule(local_vid);
      }
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, SCHEDULE_EVENT, 1);
      consensus->cancel();
    }

    
  public:
    async_consistent_engine(distributed_control &dc, 
                            graph_type& graph, 
                            const graphlab_options& opts) : 
        rmi(dc, this), graph(graph), scheduler_ptr(NULL), 
        vdata_exchange(dc),thread_barrier(opts.get_ncpus()) {
      rmi.barrier();

      // set default values
      max_clean_fraction = 1.0;
      max_clean_forks = (size_t)(-1);
      timed_termination = (size_t)(-1);
      termination_reason = execution_status::UNSET;
      set_options(opts);
      
#ifdef USE_EVENT_LOG
      PERMANENT_INITIALIZE_DIST_EVENT_LOG(eventlog, dc, std::cout, 3000, 
                                dist_event_log::RATE_BAR);
#else
      PERMANENT_INITIALIZE_DIST_EVENT_LOG(eventlog, dc, std::cout, 3000, 
                                dist_event_log::LOG_FILE);
#endif

      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, SCHEDULE_EVENT, "Schedule");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, UPDATE_EVENT, "Updates");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, WORK_ISSUED_EVENT, "Work Issued");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, INTERNAL_TASK_EVENT, "Internal");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, NO_WORK_EVENT, "No Work");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, SCHEDULE_FROM_REMOTE_EVENT, "Remote Schedule");
      PERMANENT_ADD_DIST_EVENT_TYPE(eventlog, USER_OP_EVENT, "User Ops");
      PERMANENT_ADD_IMMEDIATE_DIST_EVENT_TYPE(eventlog, ENGINE_START_EVENT, "Engine Start");
      PERMANENT_ADD_IMMEDIATE_DIST_EVENT_TYPE(eventlog, ENGINE_STOP_EVENT, "Engine Stop");
      

      INITIALIZE_TRACER(disteng_eval_sched_task, 
                        "distributed_engine: Evaluate Scheduled Task");
      INITIALIZE_TRACER(disteng_init_gathering,
                        "distributed_engine: Initialize Gather");
      INITIALIZE_TRACER(disteng_init_scattering,
                        "distributed_engine: Initialize Scattering");
      INITIALIZE_TRACER(disteng_evalfac,
                      "distributed_engine: Time in Factorized Update user code");
      INITIALIZE_TRACER(disteng_internal_task_queue,
                      "distributed_engine: Time in Internal Task Queue");
      INITIALIZE_TRACER(disteng_chandy_misra,
                      "distributed_engine: Time in Chandy Misra");
    }


    void set_options(const graphlab_options& opts) {
      rmi.barrier();
      ncpus = opts.get_ncpus();
      thread_barrier.resize_unsafe(opts.get_ncpus());
      std::vector<std::string> keys = opts.get_engine_args().get_option_keys();
      foreach(std::string opt, keys) {
        if (opt == "max_clean_fraction") {
          opts.get_engine_args().get_option("max_clean_fraction", max_clean_fraction);
        } else if (opt == "timed_termination") {
          opts.get_engine_args().get_option("timed_termination", timed_termination);
        } else {
          logstream(LOG_ERROR) << "Unexpected Engine Option: " << opt << std::endl;
        }
      }
      opts_copy = opts;
      // set a default scheduler if none
      if (opts_copy.get_scheduler_type() == "") {
        opts_copy.set_scheduler_type("queued_fifo");
      }
      rmi.barrier();
      
    }

    void initialize() {
      // construct all the required datastructures
      // deinitialize performs the reverse 
      graph.finalize();
      if (rmi.procid() == 0) memory_info::print_usage("Before Engine Initialization");
      logstream(LOG_INFO) 
        << rmi.procid() << ": Initializing..." << std::endl;
        
      // construct scheduler passing in the copy of the options from set_options
      scheduler_ptr = scheduler_factory<message_type>::
                    new_scheduler(graph.num_local_vertices(),
                                  opts_copy);

      // create initial fork arrangement based on the alternate vid mapping
      cmlocks = new distributed_chandy_misra<graph_type>(rmi.dc(), graph,
                                                        boost::bind(&engine_type::lock_ready, this, _1),
                                                        boost::bind(&engine_type::forward_cached_schedule, this, _1));

      cmlocks->compute_initial_fork_arrangement();
      
      // construct the vertex programs
      vstate.resize(graph.num_local_vertices());
      
      // construct the termination consensus object
      consensus = new async_consensus(rmi.dc(), ncpus);
      
      // finally, the thread local queues
      
      thrlocal.resize(ncpus);
      if (rmi.procid() == 0) memory_info::print_usage("After Engine Initialization");
      rmi.barrier();
    }
    
    ~async_consistent_engine() {
      thrlocal.clear();
      delete consensus;
      vstate.clear();
      delete cmlocks;
      delete scheduler_ptr;
    }
    
    /**
     * \brief Force engine to terminate immediately.
     *
     * This function is used to stop the engine execution by forcing
     * immediate termination. 
     */
    void stop() { 
      timeout = true; 
      termination_reason = execution_status::FORCED_ABORT;
    }

    
    /**
     * \brief Describe the reason for termination.
     *
     * Return the reason for the last termination.
     */
    execution_status::status_enum last_exec_status() const { 
      return termination_reason; 
    }
   
    /**
     * \brief Get the number of updates executed by the engine.
     *
     * This function returns the numbe of updates executed by the last
     * run of this engine.
     * 
     * \return the total number of updates
     */
    size_t num_updates() const { 
      return programs_executed.value; 
    }
           
    void signal_from_remote(vertex_id_type vid,
                            const message_type& message) {
      if (timeout) return;
      const lvid_type local_vid = graph.local_vid(vid);
      BEGIN_TRACEPOINT(disteng_scheduler_task_queue);
      bool direct_injection = false;
      // if the vertex is still in the locking state
      // it has not received the message yet.
      // lets directly inject it into the vertex_state message cache
      if (vstate[local_vid].state == LOCKING) {
        vstate[local_vid].lock();
        if (vstate[local_vid].state == LOCKING) {
          vstate[local_vid].current_message += message;
          direct_injection = true;
          joined_messages.inc();
        }
        vstate[local_vid].unlock();
      }
      // if we cannot directly inject into the vertex, then we have no 
      // choice but to put the message into the scheduler
      if (direct_injection == false) {
        scheduler_ptr->schedule(local_vid, message);
      }
      END_TRACEPOINT(disteng_scheduler_task_queue);
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, SCHEDULE_FROM_REMOTE_EVENT, 1);
      consensus->cancel();
    }
    
    void signal_local(vertex_id_type local_vid ,
                        const message_type& message) {
      if (timeout) return;
      if (started) {
        BEGIN_TRACEPOINT(disteng_scheduler_task_queue);
        scheduler_ptr->schedule_from_execution_thread(thread::thread_id(),
                                                      local_vid, message);
        END_TRACEPOINT(disteng_scheduler_task_queue);
      }
      else {
        scheduler_ptr->schedule(local_vid, message);
      }
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, SCHEDULE_EVENT, 1);
      consensus->cancel();
    }

    /**
     * \brief Adds an update task with a particular priority.
     * This function is forwarded to the scheduler.
     */
    void signal(const vertex_type& vtx,
                const message_type& message = message_type()) {
      signal_local(vtx.local_id(), message);
    } // end of schedule


    /**
     * \brief Creates a collection of tasks on all the vertices in the
     * graph, with the same update function and priority This function
     * is forwarded to the scheduler. Must be called by all machines
     * simultaneously
     */
    void signal_all(const message_type& message = message_type(),
                    const std::string& order = "shuffle") {
      logstream(LOG_DEBUG) << rmi.procid() << ": Schedule All" << std::endl;
      // allocate a vector with all the local owned vertices
      // and schedule all of them. 
      std::vector<vertex_id_type> vtxs;
      vtxs.reserve(graph.num_local_own_vertices());
      for(lvid_type lvid = 0; 
          lvid < graph.get_local_graph().num_vertices(); 
          ++lvid) {
        if (graph.l_vertex(lvid).owner() == rmi.procid()) {
          vtxs.push_back(lvid);        
        }
      } 
      
      if(order == "shuffle") {
        graphlab::random::shuffle(vtxs.begin(), vtxs.end());
      }
      foreach(lvid_type lvid, vtxs) {
        scheduler_ptr->schedule(lvid, message);    
      }
      rmi.barrier();
    } // end of schedule all


    // lock ready call back from the scope lock implementation
    void lock_ready(vertex_id_type lvid) {
      logstream(LOG_DEBUG) << "Lock ready on " << "L" << lvid << std::endl;
      vstate[lvid].lock();
      vstate[lvid].state = GATHERING;
      do_init_gather(lvid);
      vstate[lvid].unlock();
      master_broadcast_gathering(lvid, vstate[lvid].vertex_program);
    }

    
    void get_a_task(size_t threadid, 
                    bool& has_internal_task,
                    std::deque<lvid_type>& internal_lvid,
                    bool& has_sched_msg,
                    lvid_type& sched_lvid,
                    message_type &msg) {
      has_internal_task = false;
      has_sched_msg = false;
      if (lowres_time_seconds() - engine_start_time > timed_termination) {
        return;
      }
      BEGIN_TRACEPOINT(disteng_internal_task_queue);
      if (thrlocal[threadid].get_task(internal_lvid)) {
        has_internal_task = true;
        END_TRACEPOINT(disteng_internal_task_queue);
        return;
      }
      END_TRACEPOINT(disteng_internal_task_queue);
      
      if (cmlocks->num_clean_forks() >= max_clean_forks) {
          return;
        }
 
      sched_status::status_enum stat = 
        scheduler_ptr->get_next(threadid, sched_lvid, msg);
      has_sched_msg = stat != sched_status::EMPTY;
    } // end of get a task

    void locked_gather_complete(const lvid_type lvid) {
      vstate[lvid].apply_count_down--;
      logstream(LOG_DEBUG) << rmi.procid() << ": Partial Gather Complete: " 
                    << graph.global_vid(lvid) << "(" << vstate[lvid].apply_count_down << ")" << std::endl;
      if (vstate[lvid].apply_count_down == 0) {
        logstream(LOG_DEBUG) << rmi.procid() << ": Gather Complete " 
                             << graph.global_vid(lvid) << std::endl;
        vstate[lvid].state = APPLYING;
        add_internal_task(lvid);
      }
    }
    
    void rpc_gather_complete(vertex_id_type vid, 
                             const conditional_gather_type& uf) {
      logstream(LOG_DEBUG) << rmi.procid() << ": Receiving Gather Complete of " 
                           << vid << std::endl;
      lvid_type lvid = graph.local_vid(vid);
      vstate[lvid].lock();
      vstate[lvid].combined_gather += uf;
      locked_gather_complete(lvid);
      vstate[lvid].unlock();
    }
    

    void do_apply(lvid_type lvid) { 
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, USER_OP_EVENT, 1);
      BEGIN_TRACEPOINT(disteng_evalfac);
      context_type context(*this, graph);
      
      vertex_type vertex(graph.l_vertex(lvid));
      
      logstream(LOG_DEBUG) << rmi.procid() << ": Apply On " << vertex.id() << std::endl;   
      vstate[lvid].vertex_program.apply(context, 
                                        vertex, 
                                        vstate[lvid].combined_gather.value);
      vstate[lvid].combined_gather.clear();
      END_TRACEPOINT(disteng_evalfac);
    }
    
    void do_init_gather(lvid_type lvid) {
      context_type context(*this, graph);
      vstate[lvid].vertex_program.recv_message(context, 
                                              vertex_type(graph.l_vertex(lvid)), 
                                              vstate[lvid].current_message);
      vstate[lvid].current_message = message_type();
      vstate[lvid].combined_gather.clear();
    }   
 
    void post_delta(const vertex_type& vertex, 
                    const gather_type& delta) {
      /* To Implement */
    }

    void clear_gather_cache(const vertex_type& vertex) {
      /* To Implement */
    }


    void do_gather(lvid_type lvid) { // Do gather
      BEGIN_TRACEPOINT(disteng_evalfac);
      local_vertex_type lvertex(graph.l_vertex(lvid));
      vertex_type vertex(lvertex);

      context_type context(*this, graph);
      
      edge_dir_type gatherdir = vstate[lvid].vertex_program.gather_edges(context, vertex);
      
      if(gatherdir == graphlab::IN_EDGES || 
         gatherdir == graphlab::ALL_EDGES) {
        foreach(const local_edge_type& edge, lvertex.in_edges()) {
          edge_type e(edge);
          vstate[lvid].combined_gather +=
            vstate[lvid].vertex_program.gather(context, vertex, e);
        }
      }
      if(gatherdir == graphlab::OUT_EDGES ||
         gatherdir == graphlab::ALL_EDGES) {
        foreach(const local_edge_type& edge, lvertex.out_edges()) {
          edge_type e(edge);
          vstate[lvid].combined_gather +=
            vstate[lvid].vertex_program.gather(context, vertex, e);
        }
      }
      END_TRACEPOINT(disteng_evalfac);
    }
    
    void do_scatter(lvid_type lvid) {
      BEGIN_TRACEPOINT(disteng_evalfac);
      local_vertex_type lvertex(graph.l_vertex(lvid));
      vertex_type vertex(lvertex);

      context_type context(*this, graph);
      
      edge_dir_type scatterdir = vstate[lvid].vertex_program.scatter_edges(context, vertex);
      
      if(scatterdir == graphlab::IN_EDGES || 
         scatterdir == graphlab::ALL_EDGES) {
        foreach(const local_edge_type& edge, lvertex.in_edges()) {
          edge_type e(edge);
          vstate[lvid].vertex_program.scatter(context, vertex, e);
        }
      }
      if(scatterdir == graphlab::OUT_EDGES ||
         scatterdir == graphlab::ALL_EDGES) {
        foreach(const local_edge_type& edge, lvertex.out_edges()) {
          edge_type e(edge);
          vstate[lvid].vertex_program.scatter(context, vertex, e);
        }
      }
      END_TRACEPOINT(disteng_evalfac);
    } // end of do scatter
    
    void process_gather(lvid_type lvid) {

      const vertex_id_type vid = graph.global_vid(lvid);
      logstream(LOG_DEBUG) << rmi.procid() << ": Gathering on " << vid  
                           << std::endl;
      do_gather(lvid);

      const procid_t vowner = graph.l_get_vertex_record(lvid).owner;
      if (vowner == rmi.procid()) {
        locked_gather_complete(lvid);
      } else {
        vstate[lvid].state = MIRROR_SCATTERING;
        logstream(LOG_DEBUG) << rmi.procid() << ": Send Gather Complete of " << vid  
                             << " to " << vowner << std::endl;

        rmi.remote_call(vowner,
                        &engine_type::rpc_gather_complete,
                        graph.global_vid(lvid),
                        vstate[lvid].combined_gather);

        vstate[lvid].combined_gather.clear();
      }
    }

    
    void eval_internal_task(lvid_type lvid) {
      PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, INTERNAL_TASK_EVENT, 1);
      vstate[lvid].lock();
      switch(vstate[lvid].state) {
      case NONE: 
        ASSERT_MSG(false, "Empty Internal Task");
      case LOCKING: {
          BEGIN_TRACEPOINT(disteng_chandy_misra);
          cmlocks->make_philosopher_hungry_per_replica(lvid);
          END_TRACEPOINT(disteng_chandy_misra);
          break;
      }
      case GATHERING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Internal Task: " 
                              << graph.global_vid(lvid) << ": GATHERING(" << vstate[lvid].apply_count_down << ")" << std::endl;

          process_gather(lvid);
          break;
      }
      case MIRROR_GATHERING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Internal Task: " 
                              << graph.global_vid(lvid) << ": MIRROR_GATHERING" << std::endl;
          process_gather(lvid);
          break;
        }
      case APPLYING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Internal Task: " 
                              << graph.global_vid(lvid) << ": APPLYING" << std::endl;

          do_apply(lvid);
          vstate[lvid].state = SCATTERING;
          master_broadcast_scattering(lvid,
                                      vstate[lvid].vertex_program,
                                      graph.get_local_graph().vertex_data(lvid));
          // fall through to scattering
        }
      case SCATTERING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Scattering: " 
                              << graph.global_vid(lvid) << ": SCATTERING" << std::endl;

          do_scatter(lvid);
          programs_executed.inc();
          BEGIN_TRACEPOINT(disteng_chandy_misra);
          cmlocks->philosopher_stops_eating_per_replica(lvid);
          END_TRACEPOINT(disteng_chandy_misra);

          if (vstate[lvid].hasnext) {
            // stick next back into the scheduler
            schedule_local_next(lvid);
            vstate[lvid].hasnext = false;
          } 
          vstate[lvid].state = NONE;
          break;
        }
      case MIRROR_SCATTERING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Scattering: " 
                              << graph.global_vid(lvid) << ": MIRROR_SCATTERING" << std::endl;
          do_scatter(lvid);
          vstate[lvid].state = NONE;
          cmlocks->philosopher_stops_eating_per_replica(lvid);
//          ASSERT_FALSE(vstate[lvid].hasnext);
          break;
        }
      case MIRROR_SCATTERING_AND_NEXT_LOCKING: {
          logstream(LOG_DEBUG) << rmi.procid() << ": Scattering: " 
                              << graph.global_vid(lvid) << ": MIRROR_SCATTERING_AND_LOCKING" << std::endl;
          do_scatter(lvid);
          vstate[lvid].state = LOCKING;
//          ASSERT_FALSE(vstate[lvid].hasnext);          
          cmlocks->philosopher_stops_eating_per_replica(lvid);
          cmlocks->make_philosopher_hungry_per_replica(lvid);
          break;
        }
      }
      vstate[lvid].unlock();
    } // end of eval internal task


    void add_internal_task(lvid_type lvid) {
      if (timeout) return;
      BEGIN_TRACEPOINT(disteng_internal_task_queue);
      size_t i = lvid % ncpus;
      if (vstate[lvid].state == APPLYING || vstate[lvid].state == SCATTERING ||
          vstate[lvid].state == MIRROR_SCATTERING ||
          vstate[lvid].state == MIRROR_SCATTERING_AND_NEXT_LOCKING) {
        thrlocal[i].add_task_priority(lvid);
      }
      else {
        thrlocal[i].add_task(lvid); 
      }
      consensus->cancel_one(i);
      END_TRACEPOINT(disteng_internal_task_queue);
    }

    void forward_cached_schedule(vertex_id_type lvid) {
      message_type msg;
      const typename graph_type::vertex_record& rec = graph.l_get_vertex_record(lvid);
      if (rec.owner != rmi.procid()) {
        if (scheduler_ptr->get_specific(lvid, msg) == sched_status::NEW_TASK) {
          rmi.remote_call(rec.owner, &engine_type::signal_from_remote, rec.gvid, msg);
        }
      }
    }

    void rpc_begin_locking(vertex_id_type sched_vid) {
      logstream(LOG_DEBUG) << rmi.procid() << ": Mirror Begin Locking: " 
                           << sched_vid << std::endl;
      // immediately begin issuing the lock requests
      vertex_id_type sched_lvid = graph.local_vid(sched_vid);
      // set the vertex state
      vstate[sched_lvid].lock();
      if (vstate[sched_lvid].state == NONE) {
        vstate[sched_lvid].state = LOCKING;
        add_internal_task(sched_lvid);
      }
      else if (vstate[sched_lvid].state == MIRROR_SCATTERING) {
        vstate[sched_lvid].state = MIRROR_SCATTERING_AND_NEXT_LOCKING;
      }
/*      else {
        ASSERT_TRUE(vstate[sched_lvid].state == NONE || 
                    vstate[sched_lvid].state == MIRROR_SCATTERING);
      }*/
      vstate[sched_lvid].unlock();
    }
    
    // If I receive the call I am a mirror of this vid
    void rpc_begin_gathering(vertex_id_type sched_vid, 
                             const vertex_program_type& prog) {
      logstream(LOG_DEBUG) << rmi.procid() << ": Mirror Begin Gathering: " 
                           << sched_vid << std::endl;
//      ASSERT_NE(graph.get_vertex_record(sched_vid).owner, rmi.procid());
      // immediately begin issuing the lock requests
      vertex_id_type sched_lvid = graph.local_vid(sched_vid);
      // set the vertex state
      vstate[sched_lvid].lock();
//      ASSERT_EQ(vstate[sched_lvid].state, LOCKING);

      vstate[sched_lvid].state = MIRROR_GATHERING;
      vstate[sched_lvid].vertex_program = prog;
      vstate[sched_lvid].combined_gather.clear();
      vstate[sched_lvid].unlock();
      // lets go
      add_internal_task(sched_lvid);
    }

    /**
     * Task was added to the vstate. Now to begin scheduling the gathers
     */
    void master_broadcast_locking(lvid_type sched_lvid) {
      local_vertex_type lvertex(graph.l_vertex(sched_lvid));
      
      logstream(LOG_DEBUG) << rmi.procid() << ": Broadcast Gathering: " 
                           << lvertex.global_id() << std::endl;
//      ASSERT_I_AM_OWNER(sched_lvid);
     
      const unsigned char prevkey = 
        rmi.dc().set_sequentialization_key(lvertex.global_id() % 254 + 1);
      rmi.remote_call(lvertex.mirrors().begin(), lvertex.mirrors().end(),
                      &engine_type::rpc_begin_locking, lvertex.global_id());
      rmi.dc().set_sequentialization_key(prevkey);
    }

    /**
     * Task was added to the vstate. Now to begin scheduling the gathers
     */
    void master_broadcast_gathering(lvid_type sched_lvid,
                                    const vertex_program_type& prog) {
      local_vertex_type lvertex(graph.l_vertex(sched_lvid));
      BEGIN_TRACEPOINT(disteng_init_gathering);
      logstream(LOG_DEBUG) << rmi.procid() << ": Broadcast Gathering: " 
                           << lvertex.global_id() << std::endl;
//      ASSERT_I_AM_OWNER(sched_lvid);
      // convert to local ID
        
      const unsigned char prevkey = 
        rmi.dc().set_sequentialization_key(lvertex.global_id() % 254 + 1);
      rmi.remote_call(lvertex.mirrors().begin(), lvertex.mirrors().end(),
                      &engine_type::rpc_begin_gathering, lvertex.global_id() , prog);
      rmi.dc().set_sequentialization_key(prevkey);
      END_TRACEPOINT(disteng_init_gathering);
      add_internal_task(sched_lvid);
    }

    void rpc_begin_scattering(vertex_id_type vid, 
                              const vertex_program_type& prog,
                              const vertex_data_type &central_vdata) {
      vertex_id_type lvid = graph.local_vid(vid);
      vstate[lvid].lock();
//      ASSERT_I_AM_NOT_OWNER(lvid);
//      ASSERT_EQ(vstate[lvid].state, MIRROR_SCATTERING);
      graph.get_local_graph().vertex_data(lvid) = central_vdata;
      vstate[lvid].vertex_program = prog;
      vstate[lvid].unlock();
      add_internal_task(lvid);
    }
    
    /**
     * Task was added to the vstate. Now to begin scheduling the gathers
     */
    void master_broadcast_scattering(lvid_type sched_lvid,
                                     const vertex_program_type& prog,
                                     const vertex_data_type &central_vdata) {
      BEGIN_TRACEPOINT(disteng_init_scattering);
      local_vertex_type lvertex(graph.l_vertex(sched_lvid));
      logstream(LOG_DEBUG) << rmi.procid() << ": Broadcast Scattering: " 
                           << lvertex.global_id() << std::endl;
//      ASSERT_I_AM_OWNER(sched_lvid);

      const unsigned char prevkey = 
        rmi.dc().set_sequentialization_key(lvertex.global_id() % 254 + 1);
      rmi.remote_call(lvertex.mirrors().begin(), lvertex.mirrors().end(), 
                      &engine_type::rpc_begin_scattering, 
                      lvertex.global_id(), prog, central_vdata);
      rmi.dc().set_sequentialization_key(prevkey);
      END_TRACEPOINT(disteng_init_scattering);
    }

    template <bool prelocked>
    void eval_sched_task(const lvid_type sched_lvid, 
                         const message_type& msg) {
      BEGIN_TRACEPOINT(disteng_eval_sched_task);
      logstream(LOG_DEBUG) << rmi.procid() << ": Schedule Task: "
                           << graph.global_vid(sched_lvid) << std::endl;
      // If I am not the owner just forward the task to the other
      // scheduler and return
      const typename graph_type::vertex_record& rec = graph.l_get_vertex_record(sched_lvid);
      const procid_t owner = rec.owner;
      bool acquirelock = false;
      if (owner != rmi.procid()) {
        const vertex_id_type vid = rec.gvid;
        rmi.remote_call(owner, &engine_type::signal_from_remote, vid, msg);
        return;
      }
//      ASSERT_I_AM_OWNER(sched_lvid);
      // this is in local VIDs
      issued_messages.inc();
      if (prelocked == false) {
        vstate[sched_lvid].lock();
      }
      if (vstate[sched_lvid].state == NONE) {
        PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, UPDATE_EVENT, 1);
        PERMANENT_ACCUMULATE_DIST_EVENT(eventlog, WORK_ISSUED_EVENT, rec.num_in_edges + rec.num_out_edges);

        // we start gather right here.
        // set up the state
        vstate[sched_lvid].state = LOCKING;
        vstate[sched_lvid].hasnext = false;
        vstate[sched_lvid].current_message = msg;
        vstate[sched_lvid].apply_count_down = graph.l_vertex(sched_lvid).num_mirrors() + 1;
        acquirelock = true;
        // we are going to broadcast after unlock
      } else if (vstate[sched_lvid].state == LOCKING) {
         blocked_issues.inc();
         vstate[sched_lvid].current_message += msg;
         joined_messages.inc();
      } else {
        blocked_issues.inc();
        if (vstate[sched_lvid].hasnext) {
          scheduler_ptr->place(sched_lvid, msg);
          joined_messages.inc();
        } else {
          vstate[sched_lvid].hasnext = true;
          scheduler_ptr->place(sched_lvid, msg);
        }
      }
      if (prelocked == false) vstate[sched_lvid].unlock();
      END_TRACEPOINT(disteng_eval_sched_task);
      if (acquirelock) {
        BEGIN_TRACEPOINT(disteng_chandy_misra);
        cmlocks->make_philosopher_hungry_per_replica(sched_lvid);
        END_TRACEPOINT(disteng_chandy_misra);
        master_broadcast_locking(sched_lvid);
      }
    }
    
    
    
    
    void thread_start(size_t threadid) {
      bool has_internal_task = false;
      bool has_sched_msg = false;
      std::deque<vertex_id_type> internal_lvid;
      vertex_id_type sched_lvid;
      message_type msg;
//      size_t ctr = 0; 
      while(1) {
/*        ++ctr;
        if (max_clean_forks != (size_t)(-1) && ctr % 10000 == 0) {
          std::cout << cmlocks->num_clean_forks() << "/" << max_clean_forks << "\n";
        }*/
       get_a_task(threadid, 
                   has_internal_task, internal_lvid,
                   has_sched_msg, sched_lvid, msg);
        // if we managed to get a task..
        if (has_internal_task) {
          while(!internal_lvid.empty()) {
            eval_internal_task(internal_lvid.front());
            internal_lvid.pop_front();
          }
        } else if (has_sched_msg) {
          eval_sched_task<false>(sched_lvid, msg);
        }
        /*
         * We failed to obtain a task, try to quit
         */
        else if (!try_to_quit(threadid,
                              has_internal_task, internal_lvid,
                              has_sched_msg, sched_lvid, msg)) {
          if (has_internal_task) {
            while(!internal_lvid.empty()) {
              eval_internal_task(internal_lvid.front());
              internal_lvid.pop_front();
            }
          } else if (has_sched_msg) {
            eval_sched_task<false>(sched_lvid, msg);
          }
        } else { break; }
      }
    } // end of thread start
    
    size_t elapsed_time() const { return 1000 * (lowres_time_seconds() - engine_start_time); }
    
    size_t iteration() const { return 0; }
    
/**************************************************************************
 *                         For init vertex program                        *
***************************************************************************/
// Exchange used to swap vertex data between machines
    typedef std::pair<vertex_id_type, vertex_data_type> vid_vdata_pair_type;
    typedef buffered_exchange<vid_vdata_pair_type> vdata_exchange_type;
    vdata_exchange_type vdata_exchange;
    
    barrier thread_barrier;
    
  void sync_vertex_data(lvid_type lvid) {
    ASSERT_TRUE(graph.l_is_master(lvid));
    const vertex_id_type vid = graph.global_vid(lvid);
    local_vertex_type vertex = graph.l_vertex(lvid);
    foreach(const procid_t& mirror, vertex.mirrors()) {
      vdata_exchange.send(mirror, std::make_pair(vid, vertex.data()));
    }
  } // end of sync_vertex_data

  void recv_vertex_data() {
    procid_t procid(-1);
    typename vdata_exchange_type::buffer_type buffer;
    while(vdata_exchange.recv(procid, buffer)) {
      foreach(const vid_vdata_pair_type& pair, buffer) {
        const lvid_type lvid = graph.local_vid(pair.first);
        ASSERT_FALSE(graph.l_is_master(lvid));
        graph.l_vertex(lvid).data() = pair.second;
      }
    }
  } // end of recv vertex data
    
  void initialize_vertex_programs(size_t thread_id) {
    // For now we are using the engine as the context interface
    context_type context(*this, graph);
    for(lvid_type lvid = thread_id; lvid < graph.num_local_vertices(); 
        lvid += ncpus) {
      if(graph.l_is_master(lvid)) {          
        vertex_type vertex = local_vertex_type(graph.l_vertex(lvid));
        vstate[lvid].vertex_program.init(context, vertex);
        sync_vertex_data(lvid);
      }
      recv_vertex_data();
    }
    // Flush the buffer and finish receiving any remaining vertex
    // programs.
    thread_barrier.wait();
    if(thread_id == 0) { vdata_exchange.flush(); }
    thread_barrier.wait();
    recv_vertex_data();
  } // end of initialize_vertex_programs

    /**
     * \brief Start the engine execution.
     *
     * This \b blocking function starts the engine and does not
     * return until either one of the termination conditions evaluate
     * true or the scheduler has no tasks remaining.
     */
    void start() {
      logstream(LOG_INFO) << "Spawning " << ncpus << " threads" << std::endl;
      ASSERT_TRUE(scheduler_ptr != NULL);
      // start the scheduler
      scheduler_ptr->start();
      started = true;
      threads_alive.value = ncpus;

      rmi.barrier();

      size_t allocatedmem = memory_info::allocated_bytes();
      rmi.all_reduce(allocatedmem);

      engine_start_time = lowres_time_seconds();
      timeout = false;
      
      rmi.dc().flush_counters();
      
      termination_reason = execution_status::RUNNING;
      
        logstream(LOG_INFO) << "Initialize Vertex Programs: " << allocatedmem << std::endl;
      for (size_t i = 0; i < ncpus; ++i) {
        thrgroup.launch(boost::bind(&engine_type::initialize_vertex_programs, this, i));
      }
      thrgroup.join();
      
      
      if (rmi.procid() == 0) {
        logstream(LOG_INFO) << "Total Allocated Bytes: " << allocatedmem << std::endl;
        PERMANENT_IMMEDIATE_DIST_EVENT(eventlog, ENGINE_START_EVENT);
      }
      for (size_t i = 0; i < ncpus; ++i) {
        thrgroup.launch(boost::bind(&engine_type::thread_start, this, i));
      }
      thrgroup.join();
      
      // if termination reason was not changed, then it must be depletion
      if (termination_reason == execution_status::RUNNING) {
        termination_reason = execution_status::TASK_DEPLETION;
      }
      
      rmi.dc().flush_counters();
      if (rmi.procid() == 0) {
        PERMANENT_IMMEDIATE_DIST_EVENT(eventlog, ENGINE_STOP_EVENT);
      }
      size_t ctasks = programs_executed.value;
      rmi.all_reduce(ctasks);
      programs_executed.value = ctasks;
      
      ctasks = issued_messages.value;
      rmi.all_reduce(ctasks);
      issued_messages.value = ctasks;
      
      ctasks = blocked_issues.value;
      rmi.all_reduce(ctasks);
      blocked_issues.value = ctasks;
      
      ctasks = joined_messages.value;
      ctasks += scheduler_ptr->num_joins();
      rmi.all_reduce(ctasks);
      joined_messages.value = ctasks;
      
      if (rmi.procid() == 0) {
        std::cout << "Completed Tasks: " << programs_executed.value << std::endl;
        std::cout << "Issued Tasks: " << issued_messages.value << std::endl;
        std::cout << "Blocked Issues: " << blocked_issues.value << std::endl;
        std::cout << "------------------" << std::endl;
        std::cout << "Joined Tasks: " << joined_messages.value << std::endl;
      }

    /*for (size_t i = 0;i < vstate.size(); ++i) {
        if(vstate[i].state != NONE) {
          std::cout << "Vertex: " << i << ": " << vstate[i].state << " " << (int)(cmlocks->philosopherset[i].state) << " " << cmlocks->philosopherset[i].num_edges << " " << cmlocks->philosopherset[i].forks_acquired << "\n";
          
          foreach(typename local_graph_type::edge_type edge, cmlocks->graph.in_edges(i)) {
            std::cout << (int)(cmlocks->forkset[cmlocks->graph.edge_id(edge)]) << " ";
          }
          std::cout << "\n";
          foreach(typename local_graph_type::edge_type edge, cmlocks->graph.out_edges(i)) {
            std::cout << (int)(cmlocks->forkset[cmlocks->graph.edge_id(edge)]) << " ";
          }
          std::cout << "\n";
          getchar();
        }
      }*/
    }
  }; // end of class
} // namespace

#include <graphlab/macros_undef.hpp>

#endif // GRAPHLAB_DISTRIBUTED_ENGINE_HPP
