/******************************************************************************
 *  Copyright (c) 2008 - 2010 IBM Corporation and others.
 *  All rights reserved. This program and the accompanying materials
 *  are made available under the terms of the Eclipse Public License v1.0
 *  which accompanies this distribution, and is available at
 *  http://www.eclipse.org/legal/epl-v10.html
 * 
 *  Contributors:
 *    David Ungar, IBM Research - Initial Implementation
 *    Sam Adams, IBM Research - Initial Implementation
 *    Stefan Marr, Vrije Universiteit Brussel - Port to x86 Multi-Core Systems
 ******************************************************************************/


# include "headers.h"

void Memory_System::finished_adding_objects_from_snapshot() {
  object_table->post_store_whole_enchillada();
  The_Squeak_Interpreter()->set_am_receiving_objects_from_snapshot(false);
  enforce_coherence_after_each_core_has_stored_into_its_own_heap();
  
  // now all objects are in the heap, so we are also sure that this file is
  // in memory and the filesystem link is not to be used by mmap anymore
  OS_Interface::unlink_heap_file();
}


void Memory_System::enforce_coherence_before_each_core_stores_into_its_own_heap() {
  if (!replicate_methods &&  !replicate_all) return; // should not need this statement, but there was a bug without it, and anyway it's faster with it
  enforceCoherenceBeforeEachCoreStoresIntoItsOwnHeapMessage_class().send_to_other_cores();
  invalidate_heaps_and_fence(false);
}


void Memory_System::enforce_coherence_before_this_core_stores_into_all_heaps() {
  if (!replicate_methods &&  !replicate_all) return; // should not need this statement, but there was a bug without it, and anyway it's faster with it
  OS_Interface::mem_fence(); // ensure all cores see same heap _next's
  enforceCoherenceBeforeSenderStoresIntoAllHeapsMessage_class().send_to_other_cores();
}



bool Memory_System::verify_if(bool condition) {
  if (!condition)
    return true;
  
  // do this one at a time because the read_mostly heap messages are really flying around
  zapUnusedPortionOfHeapMessage_class().send_to_all_cores();
  
  verifyInterpreterAndHeapMessage_class().send_to_all_cores();
  
  return object_table->verify();
}

Oop Memory_System::get_stats(int what_to_sample) {
  int s = The_Squeak_Interpreter()->makeArrayStart();
  if (what_to_sample & (1 << SampleValues::gcStats)) {
    PUSH_POSITIVE_32_BIT_INT_WITH_STRING_FOR_MAKE_ARRAY(global_GC_values->gcCount);
    PUSH_POSITIVE_32_BIT_INT_WITH_STRING_FOR_MAKE_ARRAY(global_GC_values->gcMilliseconds);
    PUSH_POSITIVE_64_BIT_INT_WITH_STRING_FOR_MAKE_ARRAY(global_GC_values->gcCycles);
    global_GC_values->gcCycles = global_GC_values->gcCount = global_GC_values->gcMilliseconds = 0;
  }
  
  if (what_to_sample & (1 << SampleValues::heapStats)) {
    push_heap_stats();
  }
  
  return The_Squeak_Interpreter()->makeArray(s);
}


void Memory_System::fullGC(const char* why) {
  Squeak_Interpreter * const interp = The_Squeak_Interpreter();
  if (interp->am_receiving_objects_from_snapshot())
    fatal("cannot gc now");
  
  PERF_CNT(interp, count_full_gc());
  
  lprintf("about to fullGC: %s\n", why);
  global_GC_values->inter_gc_ms = global_GC_values->mutator_start_time ? interp->ioWhicheverMSecs() - global_GC_values->mutator_start_time : 0;
  u_int32 last_gc_start = interp->ioWhicheverMSecs();
  
  global_GC_values->gcCycles -= OS_Interface::get_cycle_count();
  
  Mark_Sweep_Collector msc;
  msc.gc();
  
  ++global_GC_values->gcCount;
  global_GC_values->gcMilliseconds += (global_GC_values->last_gc_ms = interp->ioWhicheverMSecs() - last_gc_start);
  global_GC_values->gcCycles += OS_Interface::get_cycle_count();
  
  global_GC_values->mutator_start_time = interp->ioWhicheverMSecs();
  
  level_out_heaps_if_needed();
}


void Memory_System::level_out_heaps_if_needed() {
  if (global_GC_values->inter_gc_ms  <  global_GC_values->last_gc_ms) {
    lprintf("inter_gc_ms is %d, last_gc_ms is %d; may level out\n",
            global_GC_values->inter_gc_ms, global_GC_values->last_gc_ms);
    
    Multicore_Object_Heap* biggest = biggest_heap();
    int mutability = biggest->firstAccessibleObject()->mutability();
    Multicore_Object_Heap* smallest = heaps[smallest_heap(mutability)][mutability];
    
    if (biggest->bytesUsed() * 4  >  smallest->bytesUsed() * 5) {
      lprintf("biggest bytesUsed is %d, smallest bytesUsed is %d; will level out\n",
              biggest->bytesUsed(), smallest->bytesUsed());
      
      Safepoint_for_moving_objects sf("level_out_heaps_if_needed");
      Safepoint_Ability sa(false);
      
      Object* first = biggest->firstAccessibleObject();
      Object* first_object_to_spread;
      
      for (first_object_to_spread = first; 
           first_object_to_spread  &&  ((char*)first_object_to_spread - (char*)first)  <  smallest->bytesUsed();  
           first_object_to_spread = biggest->accessibleObjectAfter(first_object_to_spread)) {}
      
      if (first_object_to_spread) {
        lprintf("Spreading objects around to prevent GC storms\n"); // by spreading only excess if needed
        The_Squeak_Interpreter()->preGCAction_everywhere(false); // false because caches are oop-based, and we just move objs
        flushFreeContextsMessage_class().send_to_all_cores();
        shuffle_or_spread_last_part_of_a_heap(first_object_to_spread, 0, Logical_Core::num_cores - 1, false, false, true);
        The_Squeak_Interpreter()->postGCAction_everywhere(false);
        The_Memory_System()->verify_if(check_many_assertions);
        lprintf("Done spreading objects around to prevent GC storms\n"); // by spreading only excess if needed
      }
    }
  }
}


Multicore_Object_Heap* Memory_System::biggest_heap() {
  Multicore_Object_Heap* biggest = NULL;
  FOR_ALL_HEAPS(rank, mutability) {
    Multicore_Object_Heap* h = heaps[rank][mutability];
    if (biggest == NULL  ||  h->bytesUsed() > biggest->bytesUsed()) 
      biggest = h; 
  }
  return biggest;
}


void Memory_System::finalize_weak_arrays_since_we_dont_do_incrementalGC() {
  fullGC("finalize_weak_arrays_since_we_dont_do_incrementalGC");
}



class Abstract_Become_Closure: public Oop_Closure {
protected:
  Object_p array1;
  Object_p array2;
  Oop *o1, *o2;
  int len;
  bool twoWay, copyHash;
  
  Abstract_Become_Closure(Object_p a1, Object_p a2, bool t, bool c) : Oop_Closure() {
    array1 = a1;
    array2 = a2;
    o1 = array1->as_oop_p() + Object::BaseHeaderSize/sizeof(Oop);
    o2 = array2->as_oop_p() + Object::BaseHeaderSize/sizeof(Oop);
    len = (a1->lastPointer() - Object::BaseHeaderSize) / sizeof(Oop)  +  1;
    twoWay = t;  copyHash = c;
  }
public:
  void copyHashes() {
    if (!copyHash) return;
    for (int i = 0;  i < len;  ++i) {
      Object_p obj1 = o1[i].as_object();  oop_int_t* hdr1p = &obj1->baseHeader;  oop_int_t hdr1 = *hdr1p;
      Object_p obj2 = o2[i].as_object();  oop_int_t* hdr2p = &obj2->baseHeader;  oop_int_t hdr2 = *hdr2p;
      if (twoWay) {
        The_Memory_System()->store_enforcing_coherence(hdr1p, (hdr1 & ~Object::HashMask)  |  (hdr2 & Object::HashMask), obj1);
      }
      The_Memory_System()->store_enforcing_coherence(hdr2p, (hdr2 & ~Object::HashMask)  |  (hdr1 & Object::HashMask), obj2);
    }
  }
  
  virtual const char* class_name() { return "Abstract_Become_Closure"; }
};

class One_Way_Become_Closure: public Abstract_Become_Closure {
public:
  One_Way_Become_Closure(Object_p a1, Object_p a2, bool c) : Abstract_Become_Closure(a1, a2, false, c) {  }
  
  void value(Oop* p, Object_p containing_obj_or_null) {
    if (containing_obj_or_null != array1)
      for (int i = 0;  i < len;  ++i)
        if (*p == o1[i]) {
          The_Memory_System()->store_enforcing_coherence_if_in_heap(p, o2[i], containing_obj_or_null);
        }
  }
  virtual const char* class_name() { return "One_Way_Become_Closure"; }
};

class Two_Way_Become_Closure: public Abstract_Become_Closure {
public:
  Two_Way_Become_Closure(Object_p a1, Object_p a2, bool c) : Abstract_Become_Closure(a1, a2, true, c) {}
  
  
  void value(Oop* p, Object_p containing_obj_or_null) {
    if (containing_obj_or_null != array1  &&  containing_obj_or_null != array2)
      for (int i = 0;  i < len;  ++i) {
        Oop x = *p;
        
        if (x == o1[i]) { The_Memory_System()->store_enforcing_coherence_if_in_heap(p, o2[i], containing_obj_or_null); }
        else if (x == o2[i]) { The_Memory_System()->store_enforcing_coherence_if_in_heap(p, o1[i], containing_obj_or_null); }
      }
  }
  virtual const char* class_name() { return "Two_Way_Become_Closure"; }
};

static bool containOnlyOops(Object_p a1, Object_p a2) {
  for (u_oop_int_t fieldOffset = a1->lastPointer() / sizeof(Oop);
       fieldOffset >= Object::BaseHeaderSize / sizeof(Oop);
       --fieldOffset )
    if (   !a1->as_oop_p()[fieldOffset].is_mem()
        || !a2->as_oop_p()[fieldOffset].is_mem() )
      return false;
  return true;
}


bool Memory_System::become_with_twoWay_copyHash(Oop array1, Oop array2, bool twoWayFlag, bool copyHashFlag) {
  Safepoint_for_moving_objects sf("become");
  Safepoint_Ability sa(false);
  
  if (!array1.isArray()  ||  !array2.isArray())  return false;
  Object_p a1o = array1.as_object();
  Object_p a2o = array2.as_object();
  if (a1o->lastPointer() != a2o->lastPointer()) return false;
  if (!containOnlyOops(a1o, a2o)) return false;
  
  Oop classP = The_Squeak_Interpreter()->splObj(Special_Indices::ClassProcess);
  for (int i = 0, n = a1o->fetchWordLength(); i < n; ++i)
    if (a1o->fetchPointer(i) == classP  ||  a2o->fetchPointer(i) == classP)
      The_Squeak_Interpreter()->set_process_object_layout_timestamp(The_Squeak_Interpreter()->process_object_layout_timestamp() + 1);
  
  
  // sync?
  if (twoWayFlag  &&  copyHashFlag) {
    swapOTEs(a1o->as_oop_p() + Object::BaseHeaderSize/sizeof(Oop),
             a2o->as_oop_p() + Object::BaseHeaderSize/sizeof(Oop),
             (a1o->lastPointer() - Object::BaseHeaderSize) / sizeof(Oop)  +  1);
    return true;
  }
  
  if (twoWayFlag) {
    Two_Way_Become_Closure bc(a1o, a2o, copyHashFlag);
    do_all_oops_including_roots_here(&bc, true); // will not do the contents of the arrays themselves
  }
  else {
    One_Way_Become_Closure bc(a1o, a2o, copyHashFlag);
    do_all_oops_including_roots_here(&bc, true); // will not do the contents of the arrays themselves
  }
  flushInterpreterCachesMessage_class().send_to_all_cores();
  return true;
}



Logical_Core* Memory_System::coreWithSufficientSpaceToAllocate(oop_int_t bytes) {
  const int mutability = Memory_System::read_write;
  Multicore_Object_Heap* h = heaps[Logical_Core::my_rank()][mutability];
  int minFree = bytes + 10000 + h->lowSpaceThreshold; // may not be necessary
  if  ( h->sufficientSpaceToAllocate(minFree) )
    return Logical_Core::my_core();
  
  if  ( heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree))
    return &logical_cores[second_chance_cores_for_allocation[mutability]];
  
  set_second_chance_cores_for_allocation(mutability);
  if  ( heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree))
    return &logical_cores[second_chance_cores_for_allocation[mutability]];
  
  if (!sufficientSpaceAfterGC(minFree, mutability))
    return NULL;
  set_second_chance_cores_for_allocation(mutability);
  if  ( heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree))
    return &logical_cores[second_chance_cores_for_allocation[mutability]];
  else return NULL;
}


bool Memory_System::sufficientSpaceAfterGC(oop_int_t minFree, int mutability) {
  The_Memory_System()->incrementalGC();
  set_second_chance_cores_for_allocation(mutability);
  
  if (heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree))
    return true;
  
  if (The_Squeak_Interpreter()->signalLowSpace())
    return false;
  fullGC("sufficientSpaceAfterGC");
  static const int extra = 15000;
  set_second_chance_cores_for_allocation(mutability);
  if (heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree + extra))
    return true;
  
  fatal("growObjectMemory");
  // oop_int_t growSize = minFree - bytesLeft() + The_Memory_System()->get_growHeadroom();
  //growObjectMemory(growSize);
  return heaps[second_chance_cores_for_allocation[mutability]][mutability]->sufficientSpaceToAllocate(minFree + extra);
}



u_int32 Memory_System::maxContiguousBytesLeft() {
  u_int32 r = 0;
  FOR_ALL_RANKS(i)
    if (heaps[i][read_write]->bytesLeft() > r)  r = heaps[i][read_write]->bytesLeft();
  return r;
}



void Memory_System::imageNameGet(Object_p dst, int len) {
  char* n = dst->as_char_p() + Object::BaseHeaderSize;
  assert(contains(n));
  
  enforce_coherence_before_store_into_object_by_interpreter(n, len, dst);
  strncpy(n, image_name, len);
  enforce_coherence_after_store_into_object_by_interpreter(n, len);
}


void Memory_System::flushExternalPrimitives() {
  FOR_ALL_HEAPS(rank, mutability) {
    heaps[rank][mutability]->flushExternalPrimitives();
  }
}

void Memory_System::handle_low_space_signals() {
  FOR_ALL_HEAPS(rank, mutability) {
    heaps[rank][mutability]->handle_low_space_signal();
  }
}


Oop Memory_System::initialInstanceOf(Oop x) {
  Oop r;
  FOR_ALL_HEAPS(rank, mutability) {
    if ((r = heaps[rank][mutability]->initialInstanceOf(x)) != The_Squeak_Interpreter()->roots.nilObj)
      return r;
  }
  return r;
}


Oop Memory_System::nextInstanceAfter(Oop x) {
  if (!x.is_mem()) return The_Squeak_Interpreter()->roots.nilObj;
  Oop klass = x.fetchClass();
  int start_rank = x.rank_of_object();
  int start_mutability = x.mutability();
  
  Oop r = heaps[start_rank][start_mutability]->next_instance_of_after(klass, x);
  if (r != The_Squeak_Interpreter()->roots.nilObj)
    return r;
  
  FOR_ALL_HEAPS(rank, mutability)
    if (&heaps[rank][mutability] > &heaps[start_rank][start_mutability]) {
      r = heaps[rank][mutability]->initialInstanceOf(klass);
      if (r != The_Squeak_Interpreter()->roots.nilObj)
        return r;
    }
  return r;
}


void Memory_System::snapshotCleanUp() {
  FOR_ALL_HEAPS(rank, mutability)
    heaps[rank][mutability]->snapshotCleanUp();
}


u_int32 Memory_System::bytesLeft() {
  u_int32 sum = 0;
  FOR_ALL_RANKS(i)
    sum += heaps[i][read_write]->bytesLeft();
  return sum;
}

int32 Memory_System::max_lastHash() {
  int r = 0;
  FOR_ALL_HEAPS(rank, mutability) {
    r = max(r, heaps[rank][mutability]->get_lastHash());
  }
  return r;
}


static const int32 headerSize = 64;




void Memory_System::writeImageFileIO(char* image_name) {
  // int32 headerStart = 0;
  FILE* f = fopen(image_name, "wb");
  if (f == NULL) {
    perror("could not open file for writing");
    The_Squeak_Interpreter()->success(false);
    return;
  }
  u_int32 heap_offsets[sizeof(heaps)/sizeof(heaps[0][0])];
  compute_snapshot_offsets(heap_offsets);
  
  write_snapshot_header(f, heap_offsets);
  
  if (!The_Squeak_Interpreter()->successFlag) {
    fclose(f);
    return;
  }
  
  if (fseek(f, headerSize, SEEK_SET)) {
    perror("seek");
    The_Squeak_Interpreter()->success(false);
    return;
  }
  bool is_first_object = true; // see comment in write_image_file
  FOR_ALL_HEAPS(rank, mutability) {
    heaps[rank][mutability]->write_image_file(f, heap_offsets, is_first_object /* passed by REF */ );
  }
  fclose(f);
  return;
}

void Memory_System::write_snapshot_header(FILE* f, u_int32* heap_offsets) {
  putLong(The_Squeak_Interpreter()->image_version, f);
  putLong(headerSize, f);
  putLong(bytesUsed() - preheader_byte_size /* Squeak 64-bit VM bug workaround */, f);
  // For explanation of preheader_byte_size above and below, see long comment about Squeak compatibility in write_image_file -- dmu 6/10
  putLong((int32)get_memory_base() + preheader_byte_size/* Squeak 64-bit VM bug workaround */, f); // start of memory;
  putLong(adjust_for_snapshot(The_Squeak_Interpreter()->roots.specialObjectsOop.as_object(), heap_offsets), f);
  putLong(max_lastHash(), f);
  int screenSize, fullScreenFlag;
  The_Interactions.get_screen_info(&screenSize, &fullScreenFlag);
  putLong(screenSize, f);
  putLong(fullScreenFlag, f);
  int32 extraVMMemory = 0;
  putLong(extraVMMemory, f);
  for (int i = 1;  i <= 7;  ++i)
    putLong(0, f);
}



void Memory_System::compute_snapshot_offsets(u_int32* offsets) {
  int last_offset = 0;
  Multicore_Object_Heap* last_heap = NULL;
  FOR_ALL_HEAPS(rank, mutability) {
    Multicore_Object_Heap** hp = &heaps[rank][mutability];
    Multicore_Object_Heap*  h = *hp;
    offsets[hp - &heaps[0][0]] =
    last_offset +=
    last_heap == NULL  ?  0
    :  (char*)h->startOfMemory() - (char*)last_heap->end_objects();
    last_heap = h;
  }
}


Oop Memory_System::firstAccessibleObject() {
  FOR_ALL_HEAPS(rank, mutability)  {
    Object* obj = heaps[rank][mutability]->firstAccessibleObject();
    if (obj != NULL)
      return obj->as_oop();
  }
  return The_Squeak_Interpreter()->roots.nilObj;
}


Oop Memory_System::nextObject(Oop x) {
  Object_p obj = x.as_object();
  int start_rank = obj->rank();
  int start_mutability = obj->mutability();
  Object* inst = heaps[start_rank][start_mutability]->accessibleObjectAfter(obj);
  if (inst != NULL)  return inst->as_oop();
  bool past_start = false;
  FOR_ALL_HEAPS(rank, mutability) {
    if (!past_start) {
      if (&heaps[rank][mutability] > &heaps[start_rank][start_mutability])
        past_start = true;
    }
    else {
      Object* inst = heaps[rank][mutability]->firstAccessibleObject();
      if (inst != NULL)
        return inst->as_oop();
    }
  }
  return  Oop::from_int(0);
}


void  Memory_System::set_lowSpaceThreshold(int32 x)  {
  FOR_ALL_HEAPS(rank, mutability)
  heaps[rank][mutability]->set_lowSpaceThreshold(x);
}

void Memory_System::initialize_main_from_buffer(void* buffer, size_t buffer_size) {
  // Each core homes its own shared Multicore_Object_Heap object
  // Each core has its own private Memory_System object
  // The actual memory for the heap is one contiguous address space, but each core homes a piece of it,
  // managed by each Multicore_Object_Heap object.
  
  
  // Create the Multicore_Object_Heap object on each core for homing.
  FOR_ALL_RANKS(i)
    if (i == Logical_Core::my_rank())
      create_my_heaps((Memory_System::init_buf*)buffer);
    else {
      logical_cores[i].message_queue.buffered_send_buffer(buffer, buffer_size);  // ensure that helper is delayed till now, even if Force_Direct_Memory_Access
      if (check_many_assertions)
        lprintf("finished sending init buffer\n");
      
      receive_heap(i);
    }
  // don't need to ilib_mem_invalidate(p, nbytes) other read_mostly heaps because we haven't written anything to them yet
  
  if (check_many_assertions)
    lprintf("finished creating all heaps\n");
  
  if (Replicate_PThread_Memory_System || Using_Processes) {
    // Now, send the addresses of these.
    FOR_ALL_OTHER_RANKS(i)
      logical_cores[i].message_queue.buffered_send_buffer(&heaps[0][0], sizeof(heaps));
  }
  
  if (check_many_assertions)
    lprintf("finished sending heaps\n");
  
  for (int i = 0;  i < max_num_mutabilities;  ++i)
    set_second_chance_cores_for_allocation(i);
  
  object_table->pre_store_whole_enchillada();
}

// TODO: the implementation of this function breaks abstraction. It should use messages instead using directly the low-level functions
void Memory_System::initialize_helper() {
  Logical_Core* sender;
  Memory_System::init_buf* ib = (Memory_System::init_buf*)Message_Queue::buffered_receive_from_anywhere(true, &sender, Logical_Core::my_core());
  
  if (Replicate_PThread_Memory_System  ||  Using_Processes)
    init_values_from_buffer(ib); // not needed with common structure
  
  if (Using_Processes)
    map_memory_on_helper(ib);
  
  create_my_heaps(ib);
  sender->message_queue.release_oldest_buffer(ib);
  
  send_local_heap();
  
  if (!Replicate_PThread_Memory_System && Using_Threads)
    return;
  
  void* heaps_buf = Message_Queue::buffered_receive_from_anywhere(true, &sender, Logical_Core::my_core());
  
  memcpy(&heaps, heaps_buf, sizeof(heaps));  
  sender->message_queue.release_oldest_buffer(heaps_buf);
  
  // don't need to ilib_mem_invalidate(p, nbytes) other read_mostly heaps because we haven't written anything to them yet
  for (int i = 0;  i < max_num_mutabilities;  ++i)
    set_second_chance_cores_for_allocation(i);
}



// three phases (for read-mostly heaps); all machines pre-cohere all heaps, then scan, the all post-cohere

// We used to do each core's heap in parallel, but when we introduced the read-mostly heap
// went back to serial, because of intercore cache-line invalidation message deadlock worries.
// xxxxxx I bet we could go back to parallel. -- dmu 4/09

void Memory_System::scan_compact_or_make_free_objects_everywhere(bool compacting, Abstract_Mark_Sweep_Collector* gc_or_null) {
  
  enforce_coherence_before_each_core_stores_into_its_own_heap();
  scanCompactOrMakeFreeObjectsMessage_class m(compacting, gc_or_null);
  m.send_to_all_cores();
  enforce_coherence_after_each_core_has_stored_into_its_own_heap();
}

u_int32 Memory_System::bytesUsed() {
  u_int32 sum = 0;
  FOR_ALL_HEAPS(rank, mutability)
  sum += heaps[rank][mutability]->bytesUsed();
  return sum;
}


void Memory_System::set_second_chance_cores_for_allocation(int mutability) {
  second_chance_cores_for_allocation[mutability] = -1;
  int max_bytesLeft = 0;
  FOR_ALL_RANKS(i) {
    int bl = heaps[i][mutability]->bytesLeft();
    if (bl > max_bytesLeft) {
      max_bytesLeft = bl;
      second_chance_cores_for_allocation[mutability] = i;
    }
  }
  assert_always(second_chance_cores_for_allocation[mutability] != -1);
}



bool Memory_System::shuffle_or_spread(int first, int last,
                                      bool move_read_write_to_read_mostly,
                                      bool move_read_mostly_to_read_write,
                                      bool spread) {
  Safepoint_for_moving_objects sf("shuffle");
  Safepoint_Ability sa(false);
  fullGC("shuffle_or_spread");
  The_Squeak_Interpreter()->preGCAction_everywhere(false); // false because caches are oop-based, and we just move objs
  flushFreeContextsMessage_class().send_to_all_cores();
  
  Object* ends[sizeof(heaps) / sizeof(heaps[0][0])];
  FOR_ALL_HEAPS(rank, mutability)
  ends[&heaps[rank][mutability] - &heaps[0][0]] = heaps[rank][mutability]->end_objects();
  
  
  FOR_ALL_HEAPS(rank, mutability) {
    Object* first_obj = heaps[rank][mutability]->first_object_or_null();
    if (first_obj == NULL)
      continue;
    if (!shuffle_or_spread_last_part_of_a_heap(first_obj, 
                                               first, last, 
                                               move_read_write_to_read_mostly, 
                                               move_read_mostly_to_read_write, 
                                               spread)) {
      The_Squeak_Interpreter()->postGCAction_everywhere(false);
      return false;
    }
  }
  The_Squeak_Interpreter()->postGCAction_everywhere(false);
  if (spread) {
    fprintf(stderr, "post spread bytes used:\n");
    print_bytes_used();
  }
  return true;
}


int32 Memory_System::smallest_heap(int mutability) {
  int result = 0;
  FOR_ALL_RANKS(rank)
    if ( heaps[result][mutability]->bytesUsed()  >  heaps[rank][mutability]->bytesUsed() )
    result = rank;
  return result;
}



bool Memory_System::shuffle_or_spread_last_part_of_a_heap(Object* first_obj,
                                                          int first, int last,
                                                          bool move_read_write_to_read_mostly,
                                                          bool move_read_mostly_to_read_write,
                                                          bool spread) {
  u_int32 old_gcCount = global_GC_values->gcCount; // cannot tolerate GCs, ends gets messed up
  Multicore_Object_Heap* source_heap = first_obj->my_heap();
  
  int num_cores = last - first + 1;
  int j = 0;
  
  for ( Object* obj = first_obj, *next = NULL;
       obj != NULL;
       obj = next ) {
    next = source_heap->next_object(obj);
    if (obj >= source_heap->end_objects() )
      break;
    else if (obj->isFreeObject())
      continue;
    int dst_mutability =
    move_read_write_to_read_mostly       ? mutability_for_posibile_replication(obj) :
    move_read_mostly_to_read_write       ? read_write  :
    obj->mutability();
    
    int dst_rank = spread ?  smallest_heap(dst_mutability)  :   j++ % num_cores  +  first;
    if (u_int32(obj->sizeBits() + 2500)  >  heaps[dst_rank][dst_mutability]->bytesLeft()) {
      return false;
    }
    else {
      obj->move_to_heap(dst_rank, dst_mutability, false);
      if (global_GC_values->gcCount != old_gcCount) {
        break;
      }
    }
  }
# warning DMU: reset end of heap here if didnt extend the heap
  return global_GC_values->gcCount == old_gcCount;
}


static const char check_mark[4] = "sqi";


void Memory_System::save_to_checkpoint(FILE* f) {
  write_mark(f, check_mark);
  
  int32 len = strlen(image_name);
  xfwrite(&len, sizeof(len), 1, f);
  xfwrite(image_name, 1, len,  f);
  
  xfwrite(&Logical_Core::group_size, sizeof(Logical_Core::group_size), 1, f);
  
  xfwrite(this, sizeof(*this), 1, f);
  
  FOR_ALL_HEAPS(rank,mutability)
  heaps[rank][mutability]->save_to_checkpoint(f);
  
  object_table->save_to_checkpoint(f);
}


void Memory_System::restore_from_checkpoint(FILE* /* f */, int /* dataSize */, int /* lastHash */, int /* savedWindowSize */, int /* fullScreenFlag */) {
# if true
  assert_always_msg(false, "deactivated checkpointing until threadsafe memory_system is ready for Tilera");
# else
  
  lprintf("restoring memory system...\n");
  read_mark(f, check_mark);
  
  int32 len;
  xfread(&len, sizeof(len), 1, f);
  char buf[BUFSIZ];
  if (len >= BUFSIZ-1) fatal("");
  xfread(buf, 1, len, f);
  // Squeak_Image_Reader::imageNamePut_on_all_cores(buf, len);
  
  int32 gs;
  xfread(&gs, sizeof(gs), 1, f);
  if (gs != Logical_Core::group_size) fatal("group_size mismatch");
  
  Memory_System::initialize_from_snapshot(dataSize, savedWindowSize, fullScreenFlag, lastHash);
  
  Memory_System local_ms;
  
  xfread(&local_ms, sizeof(local_ms), 1, f);
  
  tl->growHeadroom = local_ms.growHeadroom;
  tl->shrinkThreshold = local_ms.shrinkThreshold;
  
  FOR_ALL_HEAPS(rank,mutability)
  heaps[rank][mutability]->restore_from_checkpoint(f);
  
  
  if (tl->read_write_memory_base != local_ms.read_write_memory_base) fatal("read_write_memory_base mismatch");
  if (tl->read_write_memory_past_end != local_ms.read_write_memory_past_end) fatal("memory_past_end mismatch");
  if (page_size_used_in_heap != local_ms.page_size_used_in_heap) fatal("page_size_used_in_heap mismatch");
  
  bcopy(local_ms.second_chance_cores_for_allocation,
        tl->second_chance_cores_for_allocation,
        sizeof(second_chance_cores_for_allocation));
  tl->gcCount = local_ms.gcCount;
  tl->gcMilliseconds = local_ms.gcMilliseconds;
  tl->gcCycles = local_ms.gcCycles;
  
  object_table->restore_from_checkpoint(f);
  
  finished_adding_objects_from_snapshot();
# endif
}


void Memory_System::enforce_coherence_before_store_into_object_by_interpreter(void* p, int /* nbytes */, Object_p dst_obj_to_be_evacuated) {
  // to avoid deadlock caused by asking other cores to invalidate lines in the middle of interpreter and not being able to gc when another core asks me,
  // just move this object to read-write heap afterwards. Don't do enforce_coherence_before_store stuff.
  assert(contains(p));
  if (is_address_read_mostly(p))
    The_Squeak_Interpreter()->remember_to_move_mutated_read_mostly_object(dst_obj_to_be_evacuated->as_oop());
}


void Memory_System::pre_cohere(void* start, int nbytes) {
  if (nbytes == 0)  return;
  if (The_Squeak_Interpreter()->am_receiving_objects_from_snapshot()) return; // will be done at higher level
  // lprintf("pre_cohere start 0x%x %d\n", start, nbytes);
  
  if (!contains(start) && !object_table->probably_contains(start)) {
    lprintf("pid %d, about_to_write_read_mostly_memory to bad address 0x%x 0x%x\n", getpid(), start, nbytes);
    fatal();
  }
  aboutToWriteReadMostlyMemoryMessage_class(start, nbytes).send_to_other_cores();
  
  // lprintf("pre_cohere end\n");
}


void Memory_System::post_cohere(void* start, int nbytes) {
  if (nbytes == 0)  return;
  if (The_Squeak_Interpreter()->am_receiving_objects_from_snapshot()) return; // will be done at higher level
  // lprintf(post_cohere start 0x%x %d\n", start, nbytes);
  OS_Interface::mem_flush(start, nbytes);
  OS_Interface::mem_fence();
  // lprintf("post_cohere end\n");
}


void Memory_System::do_all_oops_including_roots_here(Oop_Closure* oc, bool sync_with_roots)  {
  The_Interactions.do_all_roots_here(oc);
  for (int mutability = 0;  mutability < max_num_mutabilities;  ++mutability)
    FOR_ALL_RANKS(r)
      heaps[r][mutability]->do_all_oops(oc);
  if (sync_with_roots)
    The_Squeak_Interpreter()->sync_with_roots();
}

void Memory_System::print_heaps() {
  FOR_ALL_HEAPS(rank,mutability) {
    lprintf("heap %d, %d:\n", rank, mutability);
    heaps[rank][mutability]->print(stdout);
  }
}


# define DEF_SEC(T) \
void Memory_System::store_enforcing_coherence(T* p, T x, Object_p dst_obj_to_be_evacuated_or_null) { \
if (sizeof(T) == bytes_per_oop) { DEBUG_STORE_CHECK((oop_int_t*)(p), (oop_int_t)(x)); } \
assert(contains(p)); \
if (is_address_read_write(p)) { *p = x; return; } \
assert(!Safepoint_Ability::is_interpreter_able()); \
if (*p == x) return; \
pre_cohere(p, sizeof(x));  \
*p = x;  \
post_cohere(p, sizeof(x)); \
if (dst_obj_to_be_evacuated_or_null != NULL) \
The_Squeak_Interpreter()->remember_to_move_mutated_read_mostly_object(dst_obj_to_be_evacuated_or_null->as_oop()); \
}


FOR_ALL_STORE_ENFORCING_COHERENCE_FUNCTIONS(DEF_SEC)


void Memory_System::store_bytes_enforcing_coherence(void* dst, const void* src, int nbytes,   Object_p dst_obj_to_be_evacuated_or_null) {
  assert(contains(dst));
  
  DEBUG_MULTIMOVE_CHECK(dst, src, nbytes / bytes_per_oop);
  
  if (is_address_read_write(dst)) {     memmove(dst, src, nbytes);  return; }
  
  if (!Safepoint_Ability::is_interpreter_able())  memmove(dst, src, nbytes);
  else {
    pre_cohere(dst, nbytes);
    memmove(dst, src, nbytes);
    post_cohere(dst, nbytes);
  }
  if (dst_obj_to_be_evacuated_or_null != NULL)
    The_Squeak_Interpreter()->remember_to_move_mutated_read_mostly_object(dst_obj_to_be_evacuated_or_null->as_oop());
}


void Memory_System::store_2_enforcing_coherence(int32* p1, int32 i1, int32 i2,  Object_p dst_obj_to_be_evacuated_or_null) {
  assert(contains(p1));
  DEBUG_STORE_CHECK(p1, i1);
  DEBUG_STORE_CHECK(&p1[1], i2);
  if (is_address_read_write(p1)) { *p1 = i1; p1[1] = i2; return; }
  if (*p1 == i1  &&  p1[1] == i2)  return;
  
  if (!Safepoint_Ability::is_interpreter_able())   { *p1 = i1;  p1[1] = i2;  }
  else {
    pre_cohere(p1, 2 * sizeof(i1));
    *p1 = i1;  p1[1] = i2;;
    post_cohere(p1, 2 * sizeof(i1));
  }
  if (dst_obj_to_be_evacuated_or_null != NULL)
    The_Squeak_Interpreter()->remember_to_move_mutated_read_mostly_object(dst_obj_to_be_evacuated_or_null->as_oop());
}

