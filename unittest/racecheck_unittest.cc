// Copyright 2008, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,           
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY           
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 

// Author: Konstantin Serebryany <opensource@google.com> 
//
// This file contains a set of unit tests for a data race detection tool. 
//
//
//
// This test can be compiled with pthreads (default) or
// with any other library that supports threads, locks, cond vars, etc. 
// 
// To compile with pthreads: 
//   g++  racecheck_unittest.cc dynamic_annotations.cc -lpthread -g
// 
// To compile with different library: 
//   1. cp thread_wrappers_pthread.h thread_wrappers_yourlib.h
//   2. edit thread_wrappers_yourlib.h
//   3. add '-DTHREAD_WRAPPERS="thread_wrappers_yourlib.h"' to your compilation.
//
//

// This test must not include any other file specific to threading library,
// everything should be inside THREAD_WRAPPERS. 
#ifndef THREAD_WRAPPERS 
# define THREAD_WRAPPERS "thread_wrappers_pthread.h"
#endif 
#include THREAD_WRAPPERS

#include <vector>
#include <algorithm>

// The tests are
// - Stability tests (marked STAB)
// - Performance tests (marked PERF)
// - Feature tests
//   - TN (true negative) : no race exists and the tool is silent. 
//   - TP (true positive) : a race exists and reported. 
//   - FN (false negative): a race exists but not reported. 
//   - FP (false positive): no race exists but the tool reports it. 
//
// The feature tests are marked according to the behavior of helgrind 3.3.0.
//
// TP and FP tests are annotated with ANNOTATE_EXPECT_RACE, 
// so, no error reports should be seen when running under helgrind. 
//
// When some of the FP cases are fixed in helgrind we'll need 
// to update this test.
//
// Each test resides in its own namespace. 
// Namespaces are named test01, test02, ... 
// Please, *DO NOT* change the logic of existing tests nor rename them. 
// Create a new test instead. 
//
// Some tests use sleep()/usleep(). 
// This is not a synchronization, but a simple way to trigger 
// some specific behaviour of the race detector's scheduler.

// Globals and utilities used by several tests. {{{1
Mutex   MU, MU1, MU2; 
CondVar CV; 
int     COND = 0;

static bool ArgIsOne(int *arg) { return *arg == 1; };
static bool ArgIsZero(int *arg) { return *arg == 0; };

// Put everything into stderr.
#define printf(args...) fprintf(stderr, args)

// An array of threads. Create/start/join all elements at once. {{{1
class MyThreadArray {
 public:
  typedef void (*F) (void);
  MyThreadArray(F f1, F f2 = NULL, F f3 = NULL, F f4 = NULL) {
    ar_[0] = new MyThread(f1);
    ar_[1] = f2 ? new MyThread(f2) : NULL;
    ar_[2] = f3 ? new MyThread(f3) : NULL;
    ar_[3] = f4 ? new MyThread(f4) : NULL;
  }
  void Start() {
    for(int i = 0; i < 4; i++) {
      if(ar_[i]) {
        ar_[i]->Start();
      }
    }
  }

  void Join() {
    for(int i = 0; i < 4; i++) {
      if(ar_[i]) {
        ar_[i]->Join();
      }
    }
  }

  ~MyThreadArray() {
    for(int i = 0; i < 4; i++) {
      delete ar_[i];
    }
  }
 private:
  MyThread *ar_[4];
};



// test00: TN. A no-op test. {{{1
namespace test00 {
void Run() {
}
}  // namespace test00


// test01: TP. Simple race (write vs write). {{{1
namespace test01 {
int     GLOB = 0;
void Worker() {
  GLOB = 1; 
}

void Parent() {
  ThreadPool pool(1);
  pool.StartWorkers();
  pool.Add(NewCallback(Worker));
  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test01:\n");
  Parent();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test01


// test02: TN. Synchronization via CondVar. {{{1
namespace test02 {
int     GLOB = 0;
// Two write accesses to GLOB are synchronized because 
// the pair of CV.Signal() and CV.Wait() establish happens-before relation. 
//
// Waiter:                      Waker: 
// 1. COND = 0
// 2. Start(Waker)              
// 3. MU.Lock()                 a. write(GLOB)
//                              b. MU.Lock()
//                              c. COND = 1
//                         /--- d. CV.Signal()
//  4. while(COND)        /     e. MU.Unock()
//       CV.Wait(MU) <---/
//  5. MU.Unlock()
//  6. write(GLOB)

void Waker() {
  usleep(10000);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1;
  CV.Signal(); 
  MU.Unlock();
}

void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  MU.Lock();
  while(COND != 1)
    CV.Wait(&MU);
  MU.Unlock();
  GLOB = 2;
}
void Run() {
  printf("test02:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test02


// test03: TN. Synchronization via LockWhen, signaller gets there first. {{{1
namespace test03 {  
int     GLOB = 0;
// Two write accesses to GLOB are synchronized via conditional critical section. 
// Note that LockWhen() happens first (we use sleep(1) to make sure)! 
//
// Waiter:                           Waker: 
// 1. COND = 0
// 2. Start(Waker)              
//                                   a. write(GLOB)
//                                   b. MU.Lock()
//                                   c. COND = 1
//                              /--- d. MU.Unlock()
// 3. MU.LockWhen(COND==1) <---/     
// 4. MU.Unlock()
// 5. write(GLOB)

void Waker() {
  sleep(1);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  MU.Unlock(); // calls ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  MU.LockWhen(Condition(&ArgIsOne, &COND));  // calls ANNOTATE_CONDVAR_WAIT
  MU.Unlock();  // Waker is done! 

  GLOB = 2;
}
void Run() {
  printf("test03:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test03

// test04: TN. Synchronization via PCQ. {{{1
namespace test04 {
int     GLOB = 0;
ProducerConsumerQueue Q(INT_MAX); 
// Two write accesses to GLOB are separated by PCQ Put/Get. 
//
// Putter:                        Getter:
// 1. write(GLOB)                
// 2. Q.Put() ---------\          .
//                      \-------> a. Q.Get()
//                                b. write(GLOB)


void Putter() {
  GLOB = 1; 
  Q.Put(NULL);
}

void Getter() {
  Q.Get();
  GLOB = 2;
}

void Run() {
  printf("test04:\n");
  MyThreadArray t(Putter, Getter);
  t.Start(); 
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test01


// test05: FP. Synchronization via CondVar, but waiter does not block. {{{1
// Since CondVar::Wait() is not called, we get a false positive. 
namespace test05 {
int     GLOB = 0;
// Two write accesses to GLOB are synchronized via CondVar. 
// But race detector can not see it. 
// See this for details: 
// http://www.valgrind.org/docs/manual/hg-manual.html#hg-manual.effective-use. 
//
// Waiter:                                  Waker: 
// 1. COND = 0                         
// 2. Start(Waker)                          
// 3. MU.Lock()                             a. write(GLOB)
//                                          b. MU.Lock()
//                                          c. COND = 1
//                                          d. CV.Signal()
//  4. while(COND)                          e. MU.Unock()
//       CV.Wait(MU) <<< not called   
//  5. MU.Unlock()      
//  6. write(GLOB)      

void Waker() {
  GLOB = 1; 
  MU.Lock();
  COND = 1;
  CV.Signal(); 
  MU.Unlock();
}

void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  sleep(1);  // Make sure the signaller gets first.
  MU.Lock();
  while(COND != 1)
    CV.Wait(&MU);
  MU.Unlock();
  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test05:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test05


// test06: TN. Synchronization via CondVar, but Waker gets there first.  {{{1
namespace test06 {
int     GLOB = 0;
// Same as test05 but we annotated the Wait() loop. 
//
// Waiter:                                            Waker: 
// 1. COND = 0                                   
// 2. Start(Waker)                                    
// 3. MU.Lock()                                       a. write(GLOB)
//                                                    b. MU.Lock()
//                                                    c. COND = 1
//                                           /------- d. CV.Signal()
//  4. while(COND)                          /         e. MU.Unock()
//       CV.Wait(MU) <<< not called        /
//  6. ANNOTATE_CONDVAR_WAIT(CV, MU) <----/
//  5. MU.Unlock()      
//  6. write(GLOB)      

void Waker() {
  GLOB = 1; 
  MU.Lock();
  COND = 1;
  CV.Signal(); 
  MU.Unlock();
}

void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  sleep(1);  // Make sure the signaller gets first.
  MU.Lock();
  while(COND != 1)
    CV.Wait(&MU);
  ANNOTATE_CONDVAR_WAIT(&CV, &MU);
  MU.Unlock();
  GLOB = 2;
}
void Run() {
  printf("test06:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test06


// test07: TN. Synchronization via LockWhen() but Waker gets there first. {{{1
namespace test07 {  
int     GLOB = 0;
// Two write accesses to GLOB are synchronized via conditional critical section. 
// Note that LockWhen() happens after COND has been set (due to sleep(1))! 
// We have to annotate Waker with ANNOTATE_CONDVAR_SIGNAL(), otherwise 
// ANNOTATE_CONDVAR_WAIT() will succeed w/o signal. 
//
// Waiter:                           Waker: 
// 1. COND = 0
// 2. Start(Waker)              
//                                   a. write(GLOB)
//                                   b. MU.Lock()
//                                   c. COND = 1
//                              /--- d. ANNOTATE_CONDVAR_SIGNAL(&MU); 
// 3. MU.LockWhen(COND==1) <---/     e. MU.Unlock()
// 4. MU.Unlock()
// 5. write(GLOB)

void Waker() {
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  ANNOTATE_CONDVAR_SIGNAL(&MU);
  MU.Unlock(); // does not call ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  sleep(1);  // Make sure the signaller gets there first.

  MU.LockWhen(Condition(&ArgIsOne, &COND));  // calls ANNOTATE_CONDVAR_WAIT
  MU.Unlock();  // Waker is done! 

  GLOB = 2;
}
void Run() {
  printf("test07:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test07

// test08: TN. Synchronization via thread start/join. {{{1
namespace test08 {
int     GLOB = 0;
// Three accesses to GLOB are separated by thread start/join. 
//
// Parent:                        Worker:
// 1. write(GLOB)
// 2. Start(Worker) ------------>
//                                a. write(GLOB)
// 3. Join(Worker) <------------
// 4. write(GLOB)
void Worker() {
  GLOB = 2; 
}

void Parent() {
  MyThread t(Worker);
  GLOB = 1;
  t.Start();
  t.Join();
  GLOB = 3;
}
void Run() {
  printf("test08:\n");
  Parent();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test08


// test09: TP. Simple race (read vs write). {{{1
namespace test09 {
int     GLOB = 0;
// A simple data race between writer and reader. 
// Write happens after read (enforced by sleep(1)). 
// Usually, easily detectable by a race detector. 
void Writer() {
  sleep(1);
  GLOB = 3; 
}
void Reader() {
  CHECK(GLOB != -777);
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test09:\n");
  MyThreadArray t(Writer, Reader);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test09


// test10: FN. Simple race (write vs read). {{{1
namespace test10 {
int     GLOB = 0;
// A simple data race between writer and reader. 
// Write happens before Read (enforced by sleep(1)), 
// otherwise this test is the same as test09. 
// 
// Writer:                    Reader:
// 1. write(GLOB)             a. sleep(long enough so that GLOB 
//                                is most likely initialized by Writer)
//                            b. read(GLOB)
// 
//
// Eraser algorithm does not detect the race here, 
// see Section 2.2 of http://citeseer.ist.psu.edu/savage97eraser.html. 
//
void Writer() {
  GLOB = 3; 
}
void Reader() {
  sleep(1);
  CHECK(GLOB != -777);
}

void Run() {
//  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test10:\n");
  MyThreadArray t(Writer, Reader);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test10


// test11: FP. Synchronization via CondVar, 2 workers. {{{1
// This test is properly synchronized, but currently (Dec 2007) 
// helgrind reports a false positive. 
//
// Parent:                              Worker1, Worker2: 
// 1. Start(workers)                    a. read(GLOB)
// 2. MU.Lock()                         b. MU.Lock()
// 3. while(COND != 2)        /-------- c. CV.Signal()
//      CV.Wait(&MU) <-------/          d. MU.Unlock()
// 4. MU.Unlock()
// 5. write(GLOB) 
//
namespace test11 {
int     GLOB = 0;
void Worker() {
  usleep(10000);
  CHECK(GLOB != 777); 

  MU.Lock();
  COND++;
  CV.Signal();
  MU.Unlock();
}

void Parent() {
  COND = 0;

  MyThreadArray t(Worker, Worker);
  t.Start();

  MU.Lock();
  while(COND != 2) {
    CV.Wait(&MU);
  }
  MU.Unlock();

  GLOB = 2;

  t.Join();
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test11:\n");
  Parent();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test11


// test12: FP. Synchronization via Mutex, then via PCQ. {{{1
namespace test12 {
int     GLOB = 0;
// This test is properly synchronized, but currently (Dec 2007) 
// helgrind reports a false positive. 
//
// First, we write to GLOB under MU, then we synchronize via PCQ, 
// which is essentially a semaphore. 
//
// Putter:                       Getter:
// 1. MU.Lock()                  a. MU.Lock()
// 2. write(GLOB) <---- MU ----> b. write(GLOB)
// 3. MU.Unlock()                c. MU.Unlock()
// 4. Q.Put()   ---------------> d. Q.Get()
//                               e. write(GLOB)
                               
ProducerConsumerQueue Q(INT_MAX);

void Putter() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  Q.Put(NULL);
}

void Getter() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  Q.Get();
  GLOB++;
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test12:\n");
  MyThreadArray t(Putter, Getter);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test12


// test13: FP. Synchronization via Mutex, then via LockWhen. {{{1
namespace test13 { 
int     GLOB = 0;
// This test is essentially the same as test12, but uses LockWhen 
// instead of PCQ.
//
// Waker:                                     Waiter:
// 1. MU.Lock()                               a. MU.Lock()
// 2. write(GLOB) <---------- MU ---------->  b. write(GLOB)
// 3. MU.Unlock()                             c. MU.Unlock()
// 4. MU.Lock()                               .
// 5. COND = 1                                .
// 6. ANNOTATE_CONDVAR_SIGNAL -------\        .        
// 7. MU.Unlock()                     \       .
//                                     \----> d. MU.LockWhen(COND == 1)
//                                            e. MU.Unlock()
//                                            f. write(GLOB)
void Waker() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  MU.Lock();
  COND = 1;
  ANNOTATE_CONDVAR_SIGNAL(&MU);
  MU.Unlock();
}

void Waiter() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  MU.LockWhen(Condition(&ArgIsOne, &COND));
  MU.Unlock();
  GLOB++;
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test13:\n");
  COND = 0;

  MyThreadArray t(Waker, Waiter);
  t.Start();
  t.Join();

  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test13


// test14: FP. Synchronization via PCQ, reads, 2 workers. {{{1
namespace test14 {
int     GLOB = 0;
// This test is properly synchronized, but currently (Dec 2007) 
// helgrind reports a false positive. 
//
// This test is similar to test11, but uses PCQ (semaphore). 
//
// Putter2:                  Putter1:                     Getter: 
// 1. read(GLOB)             a. read(GLOB)
// 2. Q2.Put() ----\         b. Q1.Put() -----\           .
//                  \                          \--------> A. Q1.Get()
//                   \----------------------------------> B. Q2.Get()
//                                                        C. write(GLOB)
ProducerConsumerQueue Q1(INT_MAX), Q2(INT_MAX);

void Putter1() {
  CHECK(GLOB != 777);
  Q1.Put(NULL);
}
void Putter2() {
  CHECK(GLOB != 777);
  Q2.Put(NULL);
}
void Getter() {
  Q1.Get();
  Q2.Get(); 
  GLOB++;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test14:\n");
  MyThreadArray t(Getter, Putter1, Putter2);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test14


// test15: TN. Synchronization via LockWhen. One waker and 2 waiters. {{{1
namespace test15 {
// Waker:                                   Waiter1, Waiter2:
// 1. write(GLOB)
// 2. MU.Lock()
// 3. COND = 1
// 4. ANNOTATE_CONDVAR_SIGNAL ------------> a. MU.LockWhen(COND == 1)
// 5. MU.Unlock()                           b. MU.Unlock()
//                                          c. read(GLOB)

int     GLOB = 0;

void Waker() {
  GLOB = 2;

  MU.Lock();
  COND = 1;
  ANNOTATE_CONDVAR_SIGNAL(&MU);
  MU.Unlock();
};

void Waiter() {
  MU.LockWhen(Condition(&ArgIsOne, &COND));
  MU.Unlock();
  CHECK(GLOB != 777);
}


void Run() {
  COND = 0;
  printf("test15:\n");
  MyThreadArray t(Waker, Waiter, Waiter);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test15


// test16: FP. Barrier, 2 threads. {{{1
namespace test16 {
// Worker1:                                     Worker2:
// 1. MU.Lock()                                 a. MU.Lock()
// 2. write(GLOB) <------------ MU ---------->  b. write(GLOB)
// 3. MU.Unlock()                               c. MU.Unlock()
// 4. MU2.Lock()                                d. MU2.Lock()
// 5. COND--                                    e. COND--
// 6. ANNOTATE_CONDVAR_SIGNAL(MU2) >>>>>V       .
// 7. MU2.Await(COND == 0) <------------+------ f. ANNOTATE_CONDVAR_SIGNAL(MU2)
// 8. MU2.Unlock()                      V>>>>>> g. MU2.Await(COND == 0)
// 9. read(GLOB)                                h. MU2.Unlock()
//                                              i. read(GLOB)
//
//
// TODO: This way we may create too many edges in happens-before graph. 
// Arndt Mühlenfeld in his PhD (TODO: link) suggests creating special nodes in 
// happens-before graph to reduce the total number of edges. 
// See figure 3.14. 
//
//
int     GLOB = 0;
Mutex MU2; 

void Worker() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  MU2.Lock(); 
  COND--;
  ANNOTATE_CONDVAR_SIGNAL(&MU2);
  MU2.Await(Condition(&ArgIsZero, &COND));
  MU2.Unlock();

  CHECK(GLOB == 2);
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  COND = 2;
  printf("test16:\n");
  MyThreadArray t(Worker, Worker);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test16


// test17: FP. Barrier, 3 threads. {{{1
namespace test17 {
// Same as test16, but with 3 threads.
int     GLOB = 0;
Mutex MU2; 

void Worker() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();

  MU2.Lock(); 
  COND--;
  ANNOTATE_CONDVAR_SIGNAL(&MU2);
  MU2.Await(Condition(&ArgIsZero, &COND));
  MU2.Unlock();

  CHECK(GLOB == 3);
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  COND = 3;
  printf("test17:\n");
  MyThreadArray t(Worker, Worker, Worker);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test17


// test18: TN. Synchronization via Await(), signaller gets there first. {{{1
namespace test18 {  
int     GLOB = 0;
// Same as test03, but uses Mutex::Await() instead of Mutex::LockWhen(). 

void Waker() {
  sleep(1);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  MU.Unlock(); // calls ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));

  MU.Lock();
  MU.Await(Condition(&ArgIsOne, &COND));  // calls ANNOTATE_CONDVAR_WAIT
  MU.Unlock();  // Waker is done! 

  GLOB = 2;
}
void Run() {
  printf("test18:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test18

// test19: TN. Synchronization via AwaitWithTimeout(). {{{1
namespace test19 {  
int     GLOB = 0;
// Same as test18, but with AwaitWithTimeout. Do not timeout. 
void Waker() {
  sleep(1);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  MU.Unlock(); // calls ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));

  MU.Lock();
  CHECK(MU.AwaitWithTimeout(Condition(&ArgIsOne, &COND), INT_MAX));
  MU.Unlock();

  GLOB = 2;
}
void Run() {
  printf("test19:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test19

// test20: TP. Incorrect synchronization via AwaitWhen(), timeout. {{{1
namespace test20 {  
int     GLOB = 0;
// True race. We timeout in AwaitWhen.
void Waker() {
  GLOB = 1; 
  usleep(100 * 1000);
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));

  MU.Lock();
  CHECK(!MU.AwaitWithTimeout(Condition(&ArgIsOne, &COND), 100));
  MU.Unlock();

  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test20:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test20

// test21: TP. Incorrect synchronization via LockWhenWithTimeout(). {{{1
namespace test21 {  
int     GLOB = 0;
// True race. We timeout in LockWhenWithTimeout().
void Waker() {
  GLOB = 1; 
  usleep(100 * 1000);
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));

  CHECK(!MU.LockWhenWithTimeout(Condition(&ArgIsOne, &COND), 100));
  MU.Unlock();

  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test21:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test21

// test22: TP. Incorrect synchronization via CondVar::WaitWithTimeout(). {{{1
namespace test22 {  
int     GLOB = 0;
// True race. We timeout in CondVar::WaitWithTimeout().
void Waker() {
  GLOB = 1; 
  usleep(100 * 1000);
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));

  int64_t ms_left_to_wait = 100; 
  int64_t deadline_ms = GetCurrentTimeMillis() + ms_left_to_wait;
  MU.Lock();
  while(COND != 1 && ms_left_to_wait > 0) {
    CV.WaitWithTimeout(&MU, ms_left_to_wait);
    ms_left_to_wait = deadline_ms - GetCurrentTimeMillis();
  }
  MU.Unlock();

  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test22:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test22

// test23: TN. TryLock, ReaderLock, ReaderTryLock. {{{1
namespace test23 {  
// Correct synchronization with TryLock, Lock, ReaderTryLock, ReaderLock. 
int     GLOB = 0;
void Worker_TryLock() {
  for (int i = 0; i < 20; i++) {
    while (true) {
      if (MU.TryLock()) {
        GLOB++; 
        MU.Unlock();
        break;
      }
      usleep(1000);
    }
  }
}

void Worker_ReaderTryLock() {
  for (int i = 0; i < 20; i++) {
    while (true) {
      if (MU.ReaderTryLock()) {
        CHECK(GLOB != 777); 
        MU.ReaderUnlock();
        break;
      }
      usleep(1000);
    }
  }
}

void Worker_ReaderLock() {
  for (int i = 0; i < 20; i++) {
    MU.ReaderLock();
    CHECK(GLOB != 777); 
    MU.ReaderUnlock();
    usleep(1000);
  }
}

void Worker_Lock() {
  for (int i = 0; i < 20; i++) {
    MU.Lock();
    GLOB++;
    MU.Unlock();
    usleep(1000);
  }
}

void Run() {
  printf("test23:\n");
  MyThreadArray t(Worker_TryLock, 
                  Worker_ReaderTryLock, 
                  Worker_ReaderLock,
                  Worker_Lock
                  );
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test23

// test24: TN. Synchronization via ReaderLockWhen(). {{{1
namespace test24 {  
int     GLOB = 0;
// Same as test03, but uses ReaderLockWhen(). 

void Waker() {
  sleep(1);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  MU.Unlock(); // calls ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  MU.ReaderLockWhen(Condition(&ArgIsOne, &COND));
  MU.ReaderUnlock();

  GLOB = 2;
}
void Run() {
  printf("test24:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test24

// test25: TN. Synchronization via ReaderLockWhenWithTimeout(). {{{1
namespace test25 {  
int     GLOB = 0;
// Same as test24, but uses ReaderLockWhenWithTimeout(). 
// We do not timeout. 

void Waker() {
  sleep(1);  // Make sure the waiter blocks.
  GLOB = 1; 

  MU.Lock();
  COND = 1; // We are done! Tell the Waiter. 
  MU.Unlock(); // calls ANNOTATE_CONDVAR_SIGNAL;
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  CHECK(MU.ReaderLockWhenWithTimeout(Condition(&ArgIsOne, &COND), INT_MAX));
  MU.ReaderUnlock();

  GLOB = 2;
}
void Run() {
  printf("test25:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test25

// test26: TP. Incorrect synchronization via ReaderLockWhenWithTimeout(). {{{1
namespace test26 {  
int     GLOB = 0;
// Same as test25, but we timeout and incorrectly assume happens-before. 

void Waker() {
  GLOB = 1; 
  usleep(10000);
}
void Waiter() {
  ThreadPool pool(1);
  pool.StartWorkers();
  COND = 0;
  pool.Add(NewCallback(Waker));
  CHECK(!MU.ReaderLockWhenWithTimeout(Condition(&ArgIsOne, &COND), 100));
  MU.ReaderUnlock();

  GLOB = 2;
}
void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test26:\n");
  Waiter();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test26


// test27: TN. Simple synchronization via SpinLock. {{{1
namespace test27 {
int     GLOB = 0;
SpinLock MU;
void Worker() {
  MU.Lock();
  GLOB++; 
  MU.Unlock();
  usleep(10000);
}

void Run() {
  printf("test27:\n");
  MyThreadArray t(Worker, Worker, Worker, Worker);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test27


// test28: FP. Synchronization via Mutex, then PCQ. 3 threads {{{1
namespace test28 {
// Putter1:                       Getter:                         Putter2:        
// 1. MU.Lock()                                                   A. MU.Lock()
// 2. write(GLOB)                                                 B. write(GLOB)
// 3. MU.Unlock()                                                 C. MU.Unlock()
// 4. Q.Put() ---------\                                 /------- D. Q.Put()
// 5. MU.Lock()         \-------> a. Q.Get()            /         E. MU.Lock()
// 6. read(GLOB)                  b. Q.Get() <---------/          F. read(GLOB)
// 7. MU.Unlock()                 c. read(GLOB)                   G. MU.Unlock()
ProducerConsumerQueue Q(INT_MAX);
int     GLOB = 0;

void Putter() {
  MU.Lock();
  GLOB++;
  MU.Unlock();

  Q.Put(NULL);

  MU.Lock();
  CHECK(GLOB != 777);
  MU.Unlock();
}

void Getter() {
  Q.Get();
  Q.Get();
  CHECK(GLOB == 2);
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test28:\n");
  MyThreadArray t(Getter, Putter, Putter);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test28


// test29: FP. Synchronization via Mutex, then PCQ. 4 threads. {{{1
namespace test29 {
// Similar to test28, but has two Getters and two PCQs. 
ProducerConsumerQueue *Q1, *Q2;
int     GLOB = 0;

void Putter(ProducerConsumerQueue *q) {
  MU.Lock();
  GLOB++;
  MU.Unlock();

  q->Put(NULL);
  q->Put(NULL);

  MU.Lock();
  CHECK(GLOB != 777);
  MU.Unlock();

}

void Putter1() { Putter(Q1); }
void Putter2() { Putter(Q2); }

void Getter() {
  Q1->Get();
  Q2->Get();
  CHECK(GLOB == 2);
  usleep(50000); //  TODO: remove this when FP in test32 is fixed. 
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test29:\n");
  Q1 = new ProducerConsumerQueue(INT_MAX);
  Q2 = new ProducerConsumerQueue(INT_MAX);
  MyThreadArray t(Getter, Getter, Putter1, Putter2);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
  delete Q1;
  delete Q2;
}
}  // namespace test29


// test30: TN. Synchronization via 'safe' race. Writer vs multiple Readers. {{{1
namespace test30 {
// This test shows a very risky kind of synchronization which is very easy 
// to get wrong. Actually, I am not sure I've got it right. 
//
// Writer:                                 Reader1, Reader2, ..., ReaderN: 
// 1. write(GLOB[i]: i >= BOUNDARY)        a. n = BOUNDARY
// 2. ANNOTATE_SIGNAL(BOUNDARY+1) -------> b. ANNOTATE_WAIT(n)
// 3. BOUNDARY++;                          c. read(GLOB[i]: i < n)
//
// Here we have a 'safe' race on accesses to BOUNDARY and 
// no actual races on accesses to GLOB[]: 
// Writer writes to GLOB[i] where i>=BOUNDARY and then increments BOUNDARY. 
// Readers read BOUNDARY and read GLOB[i] where i<BOUNDARY. 
//
// I am not completely sure that this scheme guaranties no race between 
// accesses to GLOB since compilers and CPUs 
// are free to rearrange memory operations. 
// I am actually sure that this scheme is wrong unless we use 
// some smart memory fencing... 
//
// For this unit test we use ANNOTATE_CONDVAR_WAIT/ANNOTATE_CONDVAR_SIGNAL 
// but for real life we will need separate annotations 
// (if we ever want to annotate this synchronization scheme at all). 


const int N = 50;
static int GLOB[N];
volatile int BOUNDARY = 0;

void Writer() {
  for (int i = 0; i < N; i++) {
    CHECK(BOUNDARY == i);
    for (int j = i; j < N; j++) {
      GLOB[j] = j;
    }
    ANNOTATE_CONDVAR_SIGNAL(reinterpret_cast<void*>(BOUNDARY+1));
    BOUNDARY++;
    usleep(1000);
  }
}

void Reader() {
  int n;
  do {
    n = BOUNDARY;
    if (n == 0) continue; 
    ANNOTATE_CONDVAR_WAIT(reinterpret_cast<void*>(n),
                          reinterpret_cast<void*>(n));
    for (int i = 0; i < n; i++) {
      CHECK(GLOB[i] == i);
    }
    usleep(100);
  } while(n < N);
}

void Run() {
  ANNOTATE_EXPECT_RACE((void*)(&BOUNDARY));
  printf("test30:\n");
  MyThreadArray t(Writer, Reader, Reader, Reader);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB[N-1]);
}
}  // namespace test30


// test31: TN. Synchronization via 'safe' race. Writer vs Writer. {{{1
namespace test31 {
// This test is similar to test30, but 
// it has one Writer instead of mulitple Readers. 
//
// Writer1:                                Writer2 
// 1. write(GLOB[i]: i >= BOUNDARY)        a. n = BOUNDARY
// 2. ANNOTATE_SIGNAL(BOUNDARY+1) -------> b. ANNOTATE_WAIT(n)
// 3. BOUNDARY++;                          c. write(GLOB[i]: i < n)
//

const int N = 50;
static int GLOB[N];
volatile int BOUNDARY = 0;

void Writer1() {
  for (int i = 0; i < N; i++) {
    CHECK(BOUNDARY == i);
    for (int j = i; j < N; j++) {
      GLOB[j] = j;
    }
    ANNOTATE_CONDVAR_SIGNAL(reinterpret_cast<void*>(BOUNDARY+1));
    BOUNDARY++;
    usleep(1000);
  }
}

void Writer2() {
  int n;
  do {
    n = BOUNDARY;
    if (n == 0) continue; 
    ANNOTATE_CONDVAR_WAIT(reinterpret_cast<void*>(n),
                          reinterpret_cast<void*>(n));
    for (int i = 0; i < n; i++) {
      if(GLOB[i] == i) {
        GLOB[i]++;
      }
    }
    usleep(100);
  } while(n < N);
}

void Run() {
  ANNOTATE_EXPECT_RACE((void*)(&BOUNDARY));
  printf("test31:\n");
  MyThreadArray t(Writer1, Writer2);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB[N-1]);
}
}  // namespace test31


// test32: FP. Synchronization via thread create/join. W/R. {{{1
namespace test32 {
// This test is well synchronized but helgrind 3.3.0 reports a race. 
//
// Parent:                   Writer:               Reader:  
// 1. Start(Reader) -----------------------\       .
//                                          \      .
// 2. Start(Writer) ---\                     \     .
//                      \---> a. MU.Lock()    \--> A. sleep(long enough)
//                            b. write(GLOB)     
//                      /---- c. MU.Unlock()
// 3. Join(Writer) <---/                           
//                                                 B. MU.Lock()
//                                                 C. read(GLOB)
//                                   /------------ D. MU.Unlock()
// 4. Join(Reader) <----------------/
// 5. write(GLOB)
//
//
// The call to sleep() in Reader is not part of synchronization, 
// it is required to trigger the false positive in helgrind 3.3.0. 
//
int     GLOB = 0;

void Writer() {
  MU.Lock();
  GLOB = 1;
  MU.Unlock();
}

void Reader() {
  usleep(500000);
  MU.Lock();
  CHECK(GLOB != 777);
  MU.Unlock();
}

void Parent() {
  MyThread r(Reader);
  MyThread w(Writer);
  r.Start(); 
  w.Start();

  w.Join();  // 'w' joins first. 
  r.Join(); 

  GLOB = 2;
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test32:\n");
  Parent();
  printf("\tGLOB=%d\n", GLOB);
}

}  // namespace test32


// test33: STAB. Stress test for the number of thread sets (TSETs). {{{1
namespace test33 {
int     GLOB = 0;
// Here we access N memory locations from within log(N) threads. 
// We do it in such a way that helgrind creates nearly all possible TSETs. 
// Then we join all threads and start again (N_iter times). 
const int N_iter = 50;
const int Nlog  = 15;
const int N     = 1 << Nlog;
static int ARR[N];

void Worker() {
  MU.Lock();
  int n = ++GLOB;
  MU.Unlock();

  n %= Nlog;
  for (int i = 0; i < N; i++) {
    // ARR[i] is accessed by threads from i-th subset 
    if (i & (1 << n)) {
        CHECK(ARR[i] == 0);
    }
  }
}

void Run() {
  printf("test33:\n");

  std::vector<MyThread*> vec(Nlog);

  for (int i = 0; i < N_iter; i++) {
    // Create and start Nlog threads
    for (int i = 0; i < Nlog; i++) {
      vec[i] = new MyThread(Worker);
    }
    for (int i = 0; i < Nlog; i++) {
      vec[i]->Start();
    }
    // Join all threads. 
    for (int i = 0; i < Nlog; i++) {
      vec[i]->Join();
      delete vec[i];
    }
    printf("------------------\n");
  }

  printf("\tGLOB=%d; ARR[1]=%d; ARR[7]=%d; ARR[N-1]=%d\n", 
         GLOB, ARR[1], ARR[7], ARR[N-1]);
}
}  // namespace test33


// test34: STAB. Stress test for the number of locks sets (LSETs). {{{1
namespace test34 {
// Similar to test33, but for lock sets. 
int     GLOB = 0;
const int N_iter = 50;
const int Nlog = 10;
const int N    = 1 << Nlog;
static int ARR[N];
static Mutex *MUs[Nlog];

void Worker() {
    for (int i = 0; i < N; i++) {
      // ARR[i] is protected by MUs from i-th subset of all MUs
      for (int j = 0; j < Nlog; j++)  if (i & (1 << j)) MUs[j]->Lock();
      CHECK(ARR[i] == 0);
      for (int j = 0; j < Nlog; j++)  if (i & (1 << j)) MUs[j]->Unlock();
    }
}

void Run() {
  printf("test34:\n");
  for (int iter = 0; iter < N_iter; iter++) {
    for (int i = 0; i < Nlog; i++) {
      MUs[i] = new Mutex;
    }
    MyThreadArray t(Worker, Worker);
    t.Start();
    t.Join();
    for (int i = 0; i < Nlog; i++) {
      delete MUs[i];
    }
    printf("------------------\n");
  }
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test34


// test35: PERF. Lots of mutexes and lots of call to free().  {{{1
namespace test35 {
// Helgrind 3.3.0 has very slow in shadow_mem_make_NoAccess(). Fixed locally.
// With the fix helgrind runs this test about a minute.  
// Without the fix -- about 5 minutes. (on c2d 2.4GHz). 
//
// TODO: need to figure out the best way for performance testing. 
int **ARR; 
const int N_mu   = 10000;
const int N_free = 500000;

void Worker() {
  for (int i = 0; i < N_free; i++) 
    CHECK(777 == *ARR[i]);
}

void Run() {
  printf("test35:\n");
  std::vector<Mutex*> mus;

  ARR = new int *[N_free];
  for (int i = 0; i < N_free; i++) {
    const int c = N_free / N_mu;
    if ((i % c) == 0) {
      mus.push_back(new Mutex);
      mus.back()->Lock();
      mus.back()->Unlock();
    }
    ARR[i] = new int(777);
  }

  // Need to put all ARR[i] into shared state in order 
  // to trigger the performance bug. 
  MyThreadArray t(Worker, Worker);
  t.Start();
  t.Join();
  
  for (int i = 0; i < N_free; i++) delete ARR[i];
  delete [] ARR;
  
  for (int i = 0; i < mus.size(); i++) {
    delete mus[i];
  }
}
}  // namespace test35


// test36: FP. Synchronization via Mutex, then PCQ. 3 threads. W/W {{{1
namespace test36 {
// variation of test28 (W/W instead of W/R) 

// Putter1:                       Getter:                         Putter2:        
// 1. MU.Lock();                                                  A. MU.Lock()
// 2. write(GLOB)                                                 B. write(GLOB)
// 3. MU.Unlock()                                                 C. MU.Unlock()
// 4. Q.Put() ---------\                                 /------- D. Q.Put()
// 5. MU1.Lock()        \-------> a. Q.Get()            /         E. MU1.Lock()  
// 6. MU.Lock()                   b. Q.Get() <---------/          F. MU.Lock()   
// 7. write(GLOB)                 c. MU1.Lock()                   G. write(GLOB) 
// 8. MU.Unlock()                 d. write(GLOB)                  H. MU.Unlock() 
// 9. MU1.Unlock()                e. MU1.Unlock()                 I. MU1.Unlock()
ProducerConsumerQueue Q(INT_MAX);
int     GLOB = 0;

void Putter() {
  MU.Lock();
  GLOB++;
  MU.Unlock();

  Q.Put(NULL);

  MU1.Lock();
  MU.Lock();
  GLOB++;
  MU.Unlock();
  MU1.Unlock();
}

void Getter() {
  Q.Get();
  Q.Get();
  MU1.Lock();
  GLOB++;
  MU1.Unlock();
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test36:\n");
  MyThreadArray t(Getter, Putter, Putter);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test36


// test37: TN. Simple synchronization (write vs read). {{{1
namespace test37 {
int     GLOB = 0;
// Similar to test10, but properly locked. 
void Writer() {
  MU.Lock();
  GLOB = 3; 
  MU.Unlock();
}
void Reader() {
  sleep(1);
  MU.Lock();
  CHECK(GLOB != -777);
  MU.Unlock();
}

void Run() {
  printf("test37:\n");
  MyThreadArray t(Writer, Reader);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace test37


// test38: FP. Synchronization via Mutex, then PCQ. 4 threads. W/W {{{1
namespace test38 {
// Fusion of test29 and test36. 

// Putter1:            Putter2:           Getter1:       Getter2:
//    MU1.Lock()          MU1.Lock()                                    
//    write(GLOB)         write(GLOB)                                   
//    MU1.Unlock()        MU1.Unlock()                                  
//    Q1.Put()            Q2.Put()                                      
//    Q1.Put()            Q2.Put()                                      
//    MU1.Lock()          MU1.Lock()        Q1.Get()       Q1.Get()
//    MU2.Lock()          MU2.Lock()        Q2.Get()       Q2.Get()
//    write(GLOB)         write(GLOB)       MU2.Lock()     MU2.Lock()
//    MU2.Unlock()        MU2.Unlock()      write(GLOB)    write(GLOB)
//    MU1.Unlock()        MU1.Unlock()      MU2.Unlock()   MU2.Unlock()


ProducerConsumerQueue *Q1, *Q2;
int     GLOB = 0;

void Putter(ProducerConsumerQueue *q) {
  MU1.Lock();
  GLOB++;
  MU1.Unlock();

  q->Put(NULL);
  q->Put(NULL);

  MU1.Lock();
  MU2.Lock();
  GLOB++;
  MU2.Unlock();
  MU1.Unlock();

}

void Putter1() { Putter(Q1); }
void Putter2() { Putter(Q2); }

void Getter() {
  Q1->Get();
  Q2->Get();

  MU2.Lock();
  GLOB++;
  MU2.Unlock();

  usleep(50000); //  TODO: remove this when FP in test32 is fixed. 
}

void Run() {
  ANNOTATE_EXPECT_RACE(&GLOB);
  printf("test29:\n");
  Q1 = new ProducerConsumerQueue(INT_MAX);
  Q2 = new ProducerConsumerQueue(INT_MAX);
  MyThreadArray t(Getter, Getter, Putter1, Putter2);
  t.Start();
  t.Join();
  printf("\tGLOB=%d\n", GLOB);
  delete Q1;
  delete Q2;
}
}  // namespace test38


// testXX: {{{1
namespace testXX {
int     GLOB = 0;
void Run() {
  printf("testXX:\n");
  printf("\tGLOB=%d\n", GLOB);
}
}  // namespace testXX


// List of all tests. {{{1
typedef void (*void_func_void_t)(void);
enum TEST_FLAG{
  FEATURE          = 1 << 0, 
  STABILITY        = 1 << 1, 
  PERFORMANCE      = 1 << 2,
  EXCLUDE_FROM_ALL = 1 << 3
};

static struct {
  void_func_void_t f;
  int flags;
}all_tests [] = {
  { test00::Run, FEATURE },
  { test01::Run, FEATURE },
  { test02::Run, FEATURE },
  { test03::Run, FEATURE },
  { test04::Run, FEATURE },
  { test05::Run, FEATURE },
  { test06::Run, FEATURE },
  { test07::Run, FEATURE },
  { test08::Run, FEATURE },
  { test09::Run, FEATURE },
  { test10::Run, FEATURE },
  { test11::Run, FEATURE },
  { test12::Run, FEATURE },
  { test13::Run, FEATURE },
  { test14::Run, FEATURE },
  { test15::Run, FEATURE },
  { test16::Run, FEATURE },
  { test17::Run, FEATURE },
  { test18::Run, FEATURE },
  { test19::Run, FEATURE },
  { test20::Run, FEATURE },
  { test21::Run, FEATURE },
  { test22::Run, FEATURE },
  { test23::Run, FEATURE },
  { test24::Run, FEATURE },
  { test25::Run, FEATURE },
  { test26::Run, FEATURE },
  { test27::Run, FEATURE },
  { test28::Run, FEATURE },
  { test29::Run, FEATURE },
  { test30::Run, FEATURE },
  { test31::Run, FEATURE },
  { test32::Run, FEATURE },
  { test33::Run, STABILITY|EXCLUDE_FROM_ALL },
  { test34::Run, STABILITY|EXCLUDE_FROM_ALL },
  { test35::Run, PERFORMANCE|EXCLUDE_FROM_ALL },
  { test36::Run, FEATURE },
  { test37::Run, FEATURE },
  { test38::Run, FEATURE },
  {NULL, 0 }
};


int main(int argc, char** argv) { // {{{1
  int f_num = 0;
  if (argc == 2) {
    f_num = atoi(argv[1]);
  }
  CHECK(f_num >= 0 && f_num < sizeof(all_tests)/sizeof(all_tests[0]));
  
  if (f_num == 0) {
    for (int i = 1; all_tests[i].f; i++) {
      if(all_tests[i].flags & EXCLUDE_FROM_ALL) continue;
      all_tests[i].f();
    } 
  } else {
    CHECK(all_tests[f_num].f);
    all_tests[f_num].f();
  }
}

//vim: fm:manual
