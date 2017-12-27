/*
    This file is a part of libcds - Concurrent Data Structures library

    (C) Copyright Maxim Khizhinsky (libcds.dev@gmail.com) 2006-2017

    Source code repo: http://github.com/khizmax/libcds/
    Download: http://sourceforge.net/projects/libcds/files/

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef CDSLIB_INTRUSIVE_SPECULATIVE_PAIRING_QUEUE_H
#define CDSLIB_INTRUSIVE_SPECULATIVE_PAIRING_QUEUE_H

#include <mutex>  // std::unique_lock
#include <cds/intrusive/details/base.h>
#include <cds/sync/spinlock.h>
#include <cds/os/thread.h>
#include <cds/details/bit_reverse_counter.h>
#include <cds/intrusive/options.h>
#include <cds/opt/buffer.h>
#include <cds/opt/compare.h>
#include <cds/algo/atomic.h>
#include <cds/intrusive/details/sp_queue_base.h>

namespace cds { namespace intrusive {

        /// MSPriorityQueue related definitions
        /** @ingroup cds_intrusive_helper
        */
        namespace speculative_pairing_queue {
             //@cond
            /// Slot type
            template < class GC, typename Tag = opt::none>
            using node = cds::intrusive::sp_queue::node< GC, Tag >;

            template < typename... Options >
            using base_hook = cds::intrusive::sp_queue::base_hook< Options...>;

            template <typename NodeTraits, typename... Options >
            using traits_hook = cds::intrusive::sp_queue::traits_hook< NodeTraits, Options... >;

            template < size_t MemberOffset, typename... Options >
            using member_hook = cds::intrusive::sp_queue::member_hook< MemberOffset, Options... >;

            /// MSPriorityQueue statistics
            template <typename Counter = cds::atomicity::event_counter>
            struct stat {
                typedef Counter   event_counter ; ///< Event counter type

                event_counter   m_nEnqueCount;           ///< Count of success enque operation
                event_counter   m_nQueueCreatingCount;   ///< Count of ccd reating Queue on enque
                event_counter   m_nRepeatEnqueCount;	 ///< Count of repeat iteration

                event_counter   m_nDequeCount;           ///< Count of success deque operation
                event_counter	m_nReturnEmptyInvalid;   ///< Count of EMPTY returning because of invalid queue
                event_counter   m_nClosingQueue;		 ///< Count of closing queue(made it invalid)

                //@cond
                void onEnqueSuccess()           { ++m_nEnqueCount           ;}
                void onDequeSuccess()           { ++m_nDequeCount           ;}

                void onQueueCreate()			{ ++m_nQueueCreatingCount   ;}
                void onRepeatEnque()			{ ++m_nRepeatEnqueCount	    ;}

                void onReturnEmpty()			{ ++m_nReturnEmptyInvalid   ;}
                void onCloseQueue()				{ ++m_nClosingQueue			;}
                //@endcond

                //@cond
                /*void reset()
                {
                    m_EnqueueCount.reset();
                    m_DequeueCount.reset();
                    m_EnqueueRace.reset();
                    m_DequeueRace.reset();
                    m_AdvanceTailError.reset();
                    m_BadTail.reset();
                    m_EmptyDequeue.reset();
                }

                stat& operator +=(stat const& s)
                {
                    m_EnqueueCount += s.m_EnqueueCount.get();
                    m_DequeueCount += s.m_DequeueCount.get();
                    m_EnqueueRace += s.m_EnqueueRace.get();
                    m_DequeueRace += s.m_DequeueRace.get();
                    m_AdvanceTailError += s.m_AdvanceTailError.get();
                    m_BadTail += s.m_BadTail.get();
                    m_EmptyDequeue += s.m_EmptyDequeue.get();

                    return *this;
                }*/
                //@endcond
            };

            /// MSPriorityQueue empty statistics
            struct empty_stat {
                //@cond
                void onEnqueSuccess()           const {}
                void onDequeSuccess()           const {}

                void onQueueCreate()            const {}
                void onRepeatEnque()            const {}

                void onReturnEmpty()			const {}
                void onCloseQueue()				const {}

                void reset() {}
                empty_stat& operator +=(empty_stat const&)
                {
                    return *this;
                }
                //@endcond
            };

            /// MSQueue default traits
            struct traits
            {
                typedef speculative_pairing_queue::base_hook<>        hook;
                /// Back-off strategy
                typedef cds::backoff::empty         back_off;

                /// The functor used for dispose removed items. Default is \p opt::v::empty_disposer. This option is used for dequeuing
                typedef opt::v::empty_disposer      disposer;

                /// Item counting feature; by default, disabled. Use \p cds::atomicity::item_counter to enable item counting
                typedef atomicity::empty_item_counter   item_counter;

                /// Internal statistics (by default, disabled)
                /**
                Possible option value are: \p msqueue::stat, \p msqueue::empty_stat (the default),
                user-provided class that supports \p %msqueue::stat interface.
                */
                typedef speculative_pairing_queue::empty_stat         stat;

                /// C++ memory ordering model
                /**
                Can be \p opt::v::relaxed_ordering (relaxed memory model, the default)
                or \p opt::v::sequential_consistent (sequentially consisnent memory model).
                */
                typedef opt::v::relaxed_ordering    memory_model;

                /// Link checking, see \p cds::opt::link_checker
                static constexpr const opt::link_check_type link_checker = opt::debug_check_link;

                /// Padding for internal critical atomic data. Default is \p opt::cache_line_padding
                enum { padding = opt::cache_line_padding };
            };

            /// Metafunction converting option list to traits
            /**
                \p Options:
                - \p opt::buffer - the buffer type for heap array. Possible type are: \p opt::v::initialized_static_buffer, \p opt::v::initialized_dynamic_buffer.
                    Default is \p %opt::v::initialized_dynamic_buffer.
                    You may specify any type of value for the buffer since at instantiation time
                    the \p buffer::rebind member metafunction is called to change the type of values stored in the buffer.
                - \p opt::compare - priority compare functor. No default functor is provided.
                    If the option is not specified, the \p opt::less is used.
                - \p opt::less - specifies binary predicate used for priority compare. Default is \p std::less<T>.
                - \p opt::lock_type - lock type. Default is \p cds::sync::spin
                - \p opt::back_off - back-off strategy. Default is \p cds::backoff::yield
                - \p opt::stat - internal statistics. Available types: \p mspriority_queue::stat, \p mspriority_queue::empty_stat (the default, no overhead)
            */
            template <typename... Options>
            struct make_traits {
#   ifdef CDS_DOXYGEN_INVOKED
                typedef implementation_defined type ;   ///< Metafunction result
#   else
                typedef typename cds::opt::make_options<
                        typename cds::opt::find_type_traits< traits, Options... >::type
                        ,Options...
                >::type   type;
#   endif
            };

        }   // namespace mspriority_queue

        /// Michael & Scott array-based lock-based concurrent priority queue heap
        /** @ingroup cds_intrusive_priority_queue
            Source:
                - [2013] Henzinger,Payer,Sezgin "
                    Replacing competition with cooperation to achieve scalable lock-free FIFO queues"

            Template parameters:
            - \p T - type to be stored in the queue. The priority is a part of \p T type.
            - \p Traits - type traits. See \p speculative_pairing_queue::traits for explanation.
                It is possible to declare option-based queue with \p cds::container::speculative_pairing_queue::make_traits
                metafunction instead of \p Traits template argument.
        */
        template <typename GC, typename T, class Traits = speculative_pairing_queue::traits >
        class SPQueue
        {
        public:
            typedef GC			gc			;   ///< Garbage collector
            typedef T           value_type  ;   ///< Value type stored in the queue
            typedef Traits      traits      ;   ///< Traits template parameter

            typedef typename traits::hook hook;
            typedef typename hook::node_type    node_type;  ///< node type
            typedef typename get_node_traits< value_type, node_type, hook>::type node_traits;   ///< node traits
            typedef typename sp_queue::get_link_checker< node_type, traits::link_checker >::type link_checker;   ///< link checker
            typedef typename traits::back_off   back_off;       ///< back-off strategy
            typedef typename traits::item_counter item_counter; ///< Item counter class
            typedef typename traits::stat       stat;           ///< Internal statistics
            typedef typename traits::memory_model memory_model; ///< Memory ordering. See \p cds::opt::memory_model option

            /// Rebind template arguments
            template <typename GC2, typename T2, typename Traits2>
            struct rebind {
                typedef SPQueue< GC2, T2, Traits2 > other;   ///< Rebinding result
            };

            static constexpr const size_t c_nHazardPtrCount = 2; ///< Count of hazard pointer required for the algorithm
        protected:
            typedef typename node_type::atomic_node_ptr atomic_node_ptr;
			
			
			static_assert((std::is_same<gc, typename node_type::gc>::value),"GC and node_type::gc must be the same");
			
			
            //@cond
            /// Slot type
            typedef struct SlotType {
                atomic_node_ptr m_pHead;
                node_type* m_pLast;
                node_type* m_pRemoved;
            } Slot;
            //@endcond
            const size_t C_SIZE   = 10; ///< size
					
            //@cond
            /// Queue type
            typedef struct QueueType {
				typedef typename gc::template atomic_type<int> atomic_int;
				
                bool m_Invalid;
                atomic_int  m_Cntdeq;
                atomic_int  m_Tail;
                Slot m_pair[10];
            } Queue;
            //@endcond
			typedef typename gc::template atomic_ref<Queue> atomic_queue_ptr;
			
			item_counter        			m_ItemCounter   ;   ///< Item counter
            stat               			    m_Stat          ;   ///< internal statistics accumulator
            atomic_queue_ptr			    m_Queue			;	///< Global queue

            node_type* PICKET = new node_type();
            //atomic_node_ptr PICKET;
            node_type* DUMMY = nullptr;
        public:
            /// Constructs empty speculative pairing queue
            /**
            */
            SPQueue()
            {
                m_Queue.store(new Queue, memory_model::memory_order_relaxed);
                PICKET->m_nVer = -1;
                m_Queue.load(memory_model::memory_order_relaxed)->m_Tail.store(0, memory_model::memory_order_relaxed);
                m_Queue.load(memory_model::memory_order_relaxed)->m_Cntdeq.store(0, memory_model::memory_order_relaxed);
            }

            /// Clears priority queue and destructs the object
            ~SPQueue()
            {
                clear();
            }

            /// Inserts a item into priority queue
            /**

            */
            int i = 0;
            bool enqueue( value_type& val )
            {
                Queue* pQueue; 
                int tail = 0;
                while (true) {
                    i++;
                    pQueue = m_Queue.load(memory_model::memory_order_relaxed);
                    if (pQueue->m_Invalid) {
                        Queue* pNewQueue = createNewQueue(val);
                        if (m_Queue.compare_exchange_strong(pQueue, pNewQueue, memory_model::memory_order_relaxed,memory_model::memory_order_relaxed)) {
                            m_Stat.onQueueCreate();
                            ++m_ItemCounter;
                            return true;
                        }
                        m_Stat.onRepeatEnque();
                        continue;
                    }
					
                    tail = pQueue->m_Tail.load(memory_model::memory_order_relaxed);

                    
                    int idx = tail % C_SIZE;
                    node_type* pNode = pQueue->m_pair[idx].m_pLast;
                    
                    if (tail == idx) {
                        if (pNode == nullptr) {
                            node_type* pNewNode = node_traits::to_node_ptr( val );
							pNewNode->m_nVer = tail;
                            if (pQueue->m_pair[idx].m_pHead.compare_exchange_strong(DUMMY, pNewNode,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed)) {
                                pQueue->m_pair[idx].m_pLast = pNewNode;
                                break;
                            }
                            else {
                                if (pQueue->m_pair[idx].m_pHead == PICKET)
                                    pQueue->m_Invalid = true;
                                else
                                    pQueue->m_Tail.compare_exchange_weak(tail, tail+1,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed);

                                m_Stat.onRepeatEnque();
                                continue;
                            }
                        }
                        else {
                            if (pNode == PICKET)
                                pQueue->m_Invalid = true;
                            else
                                pQueue->m_Tail.compare_exchange_weak(tail, tail+1,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed);

                            m_Stat.onRepeatEnque();
                            continue;
                        }
                    }

                    if (pNode == nullptr) {
                        pNode = pQueue->m_pair[idx].m_pHead.load(memory_model::memory_order_relaxed);
                    }

                    if (pNode == PICKET) {
                        Queue* pNewQueue = createNewQueue(val);
                        if (m_Queue.compare_exchange_strong(pQueue, pNewQueue,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed))
                        {
                            m_Stat.onQueueCreate();
                            ++m_ItemCounter;
                            return true;
                        }

                        m_Stat.onRepeatEnque();
                        continue;
                    }

                    /*if (i == 111) {
                      //  EXPECT_TRUE(false) << "t=" << tail << " c=" << pQueue->m_Cntdeq.load(memory_model::memory_order_relaxed);
                        EXPECT_TRUE(false) << " p=" << pNode->m_nVer;
                    }*/
                    while (pNode->m_pNext.load(memory_model::memory_order_relaxed) != nullptr && pNode->m_nVer < tail)
                        pNode = pNode->m_pNext.load(memory_model::memory_order_relaxed);

                    if (pNode->m_nVer >= tail) {
                        pQueue->m_Tail.compare_exchange_weak(tail, tail + 1,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed);

                        m_Stat.onRepeatEnque();
                        continue;
                    }

                    if (pNode != PICKET) {
                        node_type* pNewNode = node_traits::to_node_ptr(val);
						pNewNode->m_nVer = tail;
                        if (pNode->m_pNext.compare_exchange_strong(DUMMY, pNewNode,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed)) {
                            pQueue->m_pair[idx].m_pLast = pNewNode;
                            break;
                        }
                    }
                    else {
                        pQueue->m_Invalid = true;
                    }
                }
                pQueue->m_Tail.compare_exchange_weak(tail, tail + 1,memory_model::memory_order_relaxed,memory_model::memory_order_relaxed);
                ++m_ItemCounter;
                m_Stat.onEnqueSuccess();
                return true;
            }

            /// Extracts item with high priority
            /**

            */
            value_type* dequeue()
            {
                Queue* pQueue = m_Queue.load(memory_model::memory_order_relaxed);
                if (pQueue->m_Invalid) {
                    m_Stat.onReturnEmpty();
                    return nullptr;
                }
                 

                int ticket = 
                /*_ATOMIC_FETCH_ADD(*/
                    pQueue->m_Cntdeq.fetch_add(1, memory_model::memory_order_relaxed);
                /*);*/
                int idx = ticket % C_SIZE;

                if (ticket >= pQueue->m_Tail && ticket == idx) {//Error in article. may be must be >=
                        node_type* tmp = nullptr;
                    if (pQueue->m_pair[idx].m_pHead.compare_exchange_strong(DUMMY, PICKET, memory_model::memory_order_relaxed,memory_model::memory_order_relaxed)) {
                        CloseQueue(pQueue, idx);
                        m_Stat.onCloseQueue();
                        return nullptr;
                    }
                }

                node_type* pNode = pQueue->m_pair[idx].m_pRemoved;
                if (pNode == nullptr)
                    pNode = pQueue->m_pair[idx].m_pHead.load(memory_model::memory_order_relaxed);

                if (pNode == PICKET) {
                    CloseQueue(pQueue, idx);
                    m_Stat.onCloseQueue();
                    return nullptr;
                }

                if (pNode->m_nVer > ticket)
                    pNode = pQueue->m_pair[idx].m_pHead.load(memory_model::memory_order_relaxed);

                while (pNode->m_nVer < ticket) {
                    if (pNode->m_pNext.load(memory_model::memory_order_relaxed) == nullptr) {
                        if (pNode->m_pNext.compare_exchange_strong(DUMMY, PICKET, memory_model::memory_order_relaxed, memory_model::memory_order_relaxed)) {
                            CloseQueue(pQueue, idx);
                            m_Stat.onCloseQueue();
                            return nullptr;
                        }
                    }
                    pNode = pNode->m_pNext.load(memory_model::memory_order_relaxed);
                    if (pNode == PICKET) {
                        CloseQueue(pQueue, idx);
                        m_Stat.onCloseQueue();
                        return nullptr;
                    }
                }
                value_type* x = node_traits::to_value_ptr(pNode);
                pQueue->m_pair[idx].m_pRemoved = pNode;
                --m_ItemCounter;
                m_Stat.onDequeSuccess();
                return x;
            }

            bool push(value_type& val){
                return enqueue(val);
            }

            value_type* pop(){
                return dequeue();
            }
            /// Clears the queue (not atomic)
            /**

            */
            void clear()
            {
                 while ( dequeue() != nullptr);
            }

            /// Checks is the priority queue is empty
            bool empty() const
            {
				Queue* pQueue = m_Queue.load(memory_model::memory_order_relaxed);
/*				for (int idx = 0; idx < C_SIZE; ++idx){
					if (pQueue->m_pair[idx].m_pRemoved != pQueue->m_pair[idx].m_pLast)
						return false;
				}
*/				return pQueue->m_Tail.load(memory_model::memory_order_relaxed) <=
                        pQueue->m_Cntdeq.load(memory_model::memory_order_relaxed);
            }

            /// Returns current size of priority queue
            size_t size() const
            {
				return m_ItemCounter.value();
				/*Queue* pQueue = m_Queue.load(memory_model::memory_order_relaxed);
                return (size_t) pQueue->m_Tail.load(memory_model::memory_order_relaxed) -
                        pQueue->m_Cntdeq.load(memory_model::memory_order_relaxed);
                */
            }

            /// Returns const reference to internal statistics
            stat const& statistics() const
            {
                return m_Stat;
            }

        protected:
            Queue* createNewQueue(value_type& x) {
                Queue* pNewQueue = new Queue();
                pNewQueue->m_Invalid = false;
                pNewQueue->m_Cntdeq = 0;
                pNewQueue->m_Tail = 1;

                node_type* pNewNode = node_traits::to_node_ptr(x);
				pNewNode->m_nVer = 0;
                pNewQueue->m_pair[0].m_pHead.store(pNewNode, memory_model::memory_order_relaxed);
                pNewQueue->m_pair[0].m_pLast = pNewNode;
                return pNewQueue;
            }

            void CloseQueue(Queue* q, int idx) {
                q->m_Invalid = true;
                q->m_pair[idx].m_pRemoved = PICKET;
            }
        };

    }} // namespace cds::intrusive

#endif // #ifndef CDSLIB_INTRUSIVE_MSPRIORITY_QUEUE_H
