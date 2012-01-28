#include <gtest/gtest.h>
#include <stdlib.h>

#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "global/global_init.h"
#include "common/config.h"
#include "common/Finisher.h"
#include "os/FileJournal.h"
#include "include/Context.h"
#include "common/Mutex.h"

Finisher *finisher;
Cond sync_cond;
char path[200];
uuid_d fsid;
bool directio = false;

// ----
Cond cond;
Mutex lock("lock");
bool done;

void wait()
{
  lock.Lock();
  while (!done)
    cond.Wait(lock);
  lock.Unlock();
}

// ----
class C_Sync {
public:
  Cond cond;
  Mutex lock;
  bool done;
  C_SafeCond *c;

  C_Sync()
    : lock("C_Sync::lock"), done(false) {
    c = new C_SafeCond(&lock, &cond, &done);
  }
  ~C_Sync() {
    lock.Lock();
    //cout << "wait" << std::endl;
    while (!done)
      cond.Wait(lock);
    //cout << "waited" << std::endl;
    lock.Unlock();
  }
};

unsigned size_mb = 200;

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  char mb[10];
  sprintf(mb, "%d", size_mb);
  g_ceph_context->_conf->set_val("osd_journal_size", mb);
  g_ceph_context->_conf->apply_changes(NULL);

  finisher = new Finisher(g_ceph_context);
  
  srand(getpid()+time(0));
  snprintf(path, sizeof(path), "/tmp/test_filejournal.tmp.%d", rand());

  ::testing::InitGoogleTest(&argc, argv);

  finisher->start();

  cout << "DIRECTIO OFF" << std::endl;
  directio = false;
  int r = RUN_ALL_TESTS();
  if (r >= 0) {
    cout << "DIRECTIO ON" << std::endl;
    directio = true;
    r = RUN_ALL_TESTS();
  }
  
  finisher->stop();

  unlink(path);
  
  return r;
}

TEST(TestFileJournal, Create) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
}

TEST(TestFileJournal, WriteSmall) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
  j.make_writeable();

  bufferlist bl;
  bl.append("small");
  j.submit_entry(1, bl, 0, new C_SafeCond(&lock, &cond, &done));
  wait();

  j.close();
}

TEST(TestFileJournal, WriteBig) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
  j.make_writeable();

  bufferlist bl;
  while (bl.length() < size_mb*1000/2) {
    char foo[1024*1024];
    memset(foo, 1, sizeof(foo));
    bl.append(foo, sizeof(foo));
  }
  j.submit_entry(1, bl, 0, new C_SafeCond(&lock, &cond, &done));
  wait();

  j.close();
}

TEST(TestFileJournal, WriteMany) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
  j.make_writeable();

  C_GatherBuilder gb(g_ceph_context, new C_SafeCond(&lock, &cond, &done));
  
  bufferlist bl;
  bl.append("small");
  uint64_t seq = 1;
  for (int i=0; i<100; i++) {
    bl.append("small");
    j.submit_entry(seq++, bl, 0, gb.new_sub());
  }

  gb.activate();

  wait();

  j.close();
}

TEST(TestFileJournal, ReplaySmall) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
  j.make_writeable();
  
  C_GatherBuilder gb(g_ceph_context, new C_SafeCond(&lock, &cond, &done));
  
  bufferlist bl;
  bl.append("small");
  j.submit_entry(1, bl, 0, gb.new_sub());
  bl.append("small");
  j.submit_entry(2, bl, 0, gb.new_sub());
  bl.append("small");
  j.submit_entry(3, bl, 0, gb.new_sub());
  gb.activate();
  wait();

  j.close();

  j.open(1);

  bufferlist inbl;
  uint64_t seq = 0;
  ASSERT_EQ(true, j.read_entry(inbl, seq));
  ASSERT_EQ(seq, 2ull);
  ASSERT_EQ(true, j.read_entry(inbl, seq));
  ASSERT_EQ(seq, 3ull);
  ASSERT_EQ(false, j.read_entry(inbl, seq));

  j.make_writeable();
  j.close();
}

TEST(TestFileJournal, WriteTrim) {
  fsid.generate_random();
  FileJournal j(fsid, finisher, &sync_cond, path, directio);
  ASSERT_EQ(0, j.create());
  j.make_writeable();

  list<C_Sync*> ls;
  
  bufferlist bl;
  char foo[1024*1024];
  memset(foo, 1, sizeof(foo));

  uint64_t seq = 1, committed = 0;

  for (unsigned i=0; i<size_mb*2; i++) {
    bl.clear();
    bl.push_back(buffer::copy(foo, sizeof(foo)));
    bl.zero();
    ls.push_back(new C_Sync);
    j.submit_entry(seq++, bl, 0, ls.back()->c);

    while (ls.size() > size_mb/2) {
      delete ls.front();
      ls.pop_front();
      committed++;
      j.committed_thru(committed);
    }
  }

  while (ls.size()) {
    delete ls.front();
    ls.pop_front();
    j.committed_thru(committed);
  }

  j.close();
}
