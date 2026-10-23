#!/usr/bin/env bash
# scripts/ai-gen-programs.sh — the scripted "prompt → .lume" producer
# (TODO P4 "AI self-verification", producer half).
#
# The machine half of the loop is scripts/verify-ai-lume.sh (judges one
# program by backend equivalence) and scripts/ai-verify-loop.sh (asserts
# verdicts over static seed sets). What the loop is missing is the
# producer: a step that turns an LLM's reply into a candidate `.lume`
# program. The real host-side producer is agent-httpd's tool dispatch —
# a model writes a program, the host drops it here. This script is the
# deterministic, CI-friendly stand-in for that host step: a list of
# program *intents* (the prompts) and one template candidate per intent.
#
# Every generated candidate carries a manifest row
#     <name>|<expected verdict: accept|reject>|<prompt>
# so scripts/ai-produce.sh can assert that the gate judged it the way
# the producer intended. Seven candidates are accept-class (plausible LLM
# programs, all on the native backends' covered surface — type check +
# interp/IR-text/libLLVM agreement is the oracle, so any drift in a
# backend shows as a reject; the sql_tx seed rides the same gate, judged
# only on sqlite builds via verify-ai-lume.sh's availability probe and
# skipped with a note elsewhere); one is deliberately reject-class: the
# statistical builtins now infer float on every backend (so the old
# "missing annotation" counter-example became a legal program), and the
# counter-example moved to the mismatch the checker still catches —
# `let p: string = tdist_cdf(...)` annotates a float value as string.
# That candidate is what the gate teaches the LLM with — the payload in
# out/ai-rejects.log.
#
# Scope guard (TODO P4): this script is not an LLM builtin, touches no
# network and needs no API key; it only writes files into
# out/ai-candidates/ (override AI_CANDIDATES_DIR). The host may replace
# it with real tool dispatch later — the contract is the manifest.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="${AI_CANDIDATES_DIR:-out/ai-candidates}"
MANIFEST="$OUT/manifest.txt"
mkdir -p "$OUT"
: > "$MANIFEST"

# gen <name> <expected: accept|reject> <prompt> <<'EOF' ... EOF
gen() {
    local name=$1 expected=$2 prompt=$3
    cat > "$OUT/$name.lume"
    printf '%s|%s|%s\n' "$name" "$expected" "$prompt" >> "$MANIFEST"
    echo "generated $OUT/$name.lume — $prompt"
}

gen gen-norm-stats accept \
    "medical p-value 计算: 打印 normcdf/tdist_cdf/chisq_cdf 在常见临界值处的 p 值, 以及 erf/gamma 辅助值" <<'EOF'
// gen-norm-stats.lume — LLM-produced "medical p-value" program.
// Statistical builtins are in the native inference tables, so an
// unannotated `let p = tdist_cdf(...)` infers float and compiles on
// every backend (see gen-bad-untyped-tdist for the still-rejected
// type-mismatch counter-example).
let p = tdist_cdf(2.228139, 10.0);
print(p);
print(normcdf(1.959963984540054));
print(normcdf(-1.959963984540054));
print(tdist_cdf(2.228139, 10.0));
print(tdist_cdf(-1.812461, 10.0));
print(chisq_cdf(5.991465, 2.0));
print(erf(1.0));
print(gamma(5.0));
EOF

gen gen-portfolio-sort accept \
    "把一组收益率排序(原地)并汇总: 打印排序后的列表、正收益部分与总和" <<'EOF'
// gen-portfolio-sort.lume — LLM-produced "sort the daily returns" program.
// sort() sorts in place and returns the same list; map/filter/reduce
// consume it afterwards.
let returns = [12.5, -3.2, 8.7, 0.4, -1.1];
let sorted = sort(returns);
print(sorted);
print(filter((x) => x > 0.0, sorted));
print(reduce((a, x) => a + x, sorted, 0.0));
print(len(sorted));
EOF

gen gen-summary-stats accept \
    "计算一组浮点样本的均值与方差: reduce 求和、map 平方差、再 reduce" <<'EOF'
// gen-summary-stats.lume — LLM-produced "summary statistics" program.
// All operations widen to float; the accumulator seeds pin the slots.
let xs = [1.0, 2.0, 3.0, 4.0, 5.0];
let total = reduce((a, x) => a + x, xs, 0.0);
let mean = total / len(xs);
let sq = map((x) => (x - mean) * (x - mean), xs);
let variance = reduce((a, x) => a + x, sq, 0.0) / len(xs);
print(mean);
print(variance);
print(mean * 2.0);
EOF

gen gen-json-marshal accept \
    "把一条嵌套记录序列化为 JSON: stringify/json/str 三个视角 + 异构列表" <<'EOF'
// gen-json-marshal.lume — LLM-produced "marshal a record to JSON" program.
// stringify() quotes a bare string, json() renders like str() over
// containers; nested maps/lists and null elements all serialize natively.
let rec = { "name": "lume", "ver": 7, "tags": ["lang", "vm"], "stats": { "ok": true } };
print(stringify(rec));
print(json(rec));
print(str(rec));
let flat = [1, "two", 3.5, true, null];
print(stringify(flat));
print(str(flat));
EOF

gen gen-enum-match accept \
    "按几何形状分拣面积: enum + match 表达式 + 三元/if 表达式比较面积" <<'EOF'
// gen-enum-match.lume — LLM-produced "shape area classifier" program.
// match is an expression; ternary and if-expressions fold to it.
enum Shape { Circle(r: float) | Rect(w: float, h: float) }

func area(s: Shape): float {
  return match (s) {
    Circle(r) => 3.0 * r * r,
    Rect(w, h) => w * h
  };
}
print(area(Circle(2.0)));
print(area(Rect(3.0, 4.0)));
print(area(Circle(1.0)) > area(Rect(2.0, 2.0)) ? "circle" : "rect");
print(if (area(Rect(2.0, 2.0)) > 4.0) "big" else "small");
EOF

gen gen-range-sieve accept \
    "埃拉托色尼筛: 嵌套 for + range + 取模, push 收集质数并打印" <<'EOF'
// gen-range-sieve.lume — LLM-produced "sieve of Eratosthenes" program.
// Nested counted for-loops, `%` modulo, a bool flag and push into a
// list; the list is returned and printed from the call site.
func sieve(n: int): int[] {
  let primes = [];
  for (let i = 2; i < n; i = i + 1) {
    let is_prime = true;
    for (let j = 2; j * j <= i; j = j + 1) {
      if (i % j == 0) { is_prime = false; }
    }
    if (is_prime) { push(primes, i); }
  }
  return primes;
}
print(sieve(20));
print(len(sieve(20)));
EOF

gen gen-sql-tx accept \
    "SQLite 事务: 开事务 -> 建表/写入 -> 查询回读 -> 提交, 打印查询结果" <<'EOF'
// gen-sql-tx.lume — LLM-produced "SQLite transaction" program (P6 surface).
// Exercises the tx path: open -> write (idempotent table + upsert) ->
// query the row back -> commit. The sql_* builtins are optional in the
// build (HAVE_SQLITE); on a build without them the gate's availability
// probe skips this seed instead of rejecting it. SQLITE_DB supplies the
// scratch database (empty path falls back to it on every backend).
let db: string = sql_tx_begin("");
sql_tx_exec(db, "CREATE TABLE IF NOT EXISTS t (k TEXT PRIMARY KEY, v TEXT)");
sql_tx_exec(db, "INSERT OR REPLACE INTO t (k, v) VALUES ('a', 'hello')");
let rows: list = sql_query("", "SELECT v FROM t WHERE k = 'a'");
print(rows);
sql_tx_commit(db);
EOF

gen gen-bad-untyped-tdist reject \
    "反例: 统计调用的类型注解与返回值不匹配 —— let p: string = tdist_cdf(...) 返回 float, 必须拒绝, counter-example 教 LLM 写对注解类型" <<'EOF'
// gen-bad-untyped-tdist.lume — the reject counter-example.
// The statistical builtins ARE in the native inference tables now
// (normcdf / erf / gamma / lgamma / igamma / ibeta / tdist_cdf /
// chisq_cdf all infer float, see gen-norm-stats), so a missing
// annotation is legal on every backend. The counter-example moved to
// the other failure mode: the annotation does not match the value —
// tdist_cdf returns a float, `let p: string` is a type error and every
// backend must refuse it.
let p: string = tdist_cdf(2.228139, 10.0);
print(p);
EOF

echo "ai-gen-programs: $(ls "$OUT"/*.lume | wc -l) candidate(s) generated in $OUT"
