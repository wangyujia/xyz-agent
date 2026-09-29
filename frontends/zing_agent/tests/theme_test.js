const fs = require('fs');
const vm = require('vm');
const html = fs.readFileSync('/root/code/thin_agent_frontends/zing_agent/web/chat.html', 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const themeJs = js.substring(0, js.indexOf('/* ══════════════ 状态'));
const store = {};
const attrs = {};
let mqDark = false;
const ctx = vm.createContext({
  localStorage: { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } },
  document: { documentElement: { setAttribute: (k, v) => { attrs[k] = v; } }, getElementById: () => null },
  window: { matchMedia: () => ({ matches: mqDark, addEventListener: () => {} }) },
});
vm.runInContext(themeJs, ctx);
// v7: kvSet 走 KV 层——先 kvLoad 激活降级写（无桥→localStorage）
vm.runInContext('if (typeof kvLoaded !== "undefined") kvLoaded = true;', ctx);
const api = () => vm.runInContext('({applyTheme, cycleTheme})', ctx);
let failed = 0;
const check = (ok, name) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + name); if (!ok) failed++; };
check(attrs['data-theme'] === 'light', '① 默认 system+亮系统 → light');
mqDark = true; api().applyTheme('system');
check(attrs['data-theme'] === 'dark', '② system+暗系统 → dark');
api().applyTheme('light');
check(attrs['data-theme'] === 'light' && store['zing_theme'] === 'light', '③ 显式 light（系统暗也浅）+记忆');
api().applyTheme('dark');
check(attrs['data-theme'] === 'dark' && store['zing_theme'] === 'dark', '④ 显式 dark+记忆');
vm.runInContext('kvMem.zing_theme = "light"', ctx); api().cycleTheme();
check(attrs['data-theme'] === 'dark' && store['zing_theme'] === 'dark', '⑤ cycle light→dark');
api().cycleTheme();
check(store['zing_theme'] === 'system', '⑥ cycle dark→system（三态回环）');
// ⑦ 重启恢复：全新上下文读同 store
const attrs2 = {};
const ctx2 = vm.createContext({
  localStorage: { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } },
  document: { documentElement: { setAttribute: (k, v) => { attrs2[k] = v; } }, getElementById: () => null },
  window: { matchMedia: () => ({ matches: true }) },
});
vm.runInContext(themeJs, ctx2);
vm.runInContext('if (typeof kvLoaded !== "undefined") kvLoaded = true;', ctx2);
check(attrs2['data-theme'] === 'dark', '⑦ 重启恢复 system+暗 → dark（防闪烁首帧即用）');
console.log(failed ? 'FAILED' : 'ALL PASS');
process.exit(failed ? 1 : 0);
