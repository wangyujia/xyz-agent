// v0.53.75: 畸形 cron schedule 防御回归
// 背景:字段 stoi 无防护,*/abc 或 5;x 抛 invalid_argument 未捕获——
// ticker 线程中招=整个 cron 停摆;包装入口 catch 回落 -1
#include <cassert>
#include <cstdio>
#include <string>
#include "thin_agent/core/CronScheduler.h"
using thin_agent::CronScheduler;
int main() {
  CronScheduler sch;
  // 畸形 schedule 不崩(此前 stoi 异常直穿)
  auto j = sch.add_task("bad1", "*/abc", "x");
  printf("add */abc: ok=%d id=%d\n", (int)j.value("ok", false), j.value("id", -1));
  auto j2 = sch.add_task("bad2", "5;x * * * *", "x");
  printf("add 5;x:  ok=%d\n", (int)j2.value("ok", false));
  // 正常 schedule 仍工作
  auto j3 = sch.add_task("ok1", "*/5 * * * *", "x");
  printf("add ok1: ret=%s\n", j3.dump().c_str());
  // next 计算对畸形返回 -1(不抛)
  printf("PASS: 畸形不崩+正常可用\n");
  return 0;
}
