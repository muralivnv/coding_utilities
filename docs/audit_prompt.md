Source code `koi/src` is a 17K line C++23 text editor with ZED editor like multi-buffers, treesitter based code navigation, sqlite-database based state management.
I need a deeply adversarial, highly thorough code audit for changes made from commit 93166f6 to HEAD (including commit 93166f6).

Instructions:
Review the codebase for bugs, edge cases, and performance bottlenecks. Focus only on the changes made in commits
93166f6 to HEAD. The original design for smart_jump can be see at docs/smart-jump.md
Attack the logic adversarially.
Focus your audit on these specific vectors:
- Memory & C++ Footguns: Out-of-bounds accesses, one-off indexing errors, `nullptr` dereferences, memory leaks, and lifetime issues (e.g., dangling string_views/spans, undefined behavior).
- Critical Paths: Database calls, file I/O, threading, fuzzy calculation, healing logic.
  - Pinpoint areas lacking robust error handling (e.g., missing `try...catch`, unchecked `std::expected` or `std::optional`, or missing fallback logic).
  - Suggest concrete implementations to prevent crashes.
- State Corruption: Logical bugs that can lead to internal state corruption.
- Review each file using a dedicated agent with a dedicated prompt created for opus 5. YOU MUST DELEGATE and you never should read the entire file.
- Every finding you receive, you verify it again to filter out false positives.
- Make sure we only run 3 agents at any given time as I've seen in the past running more than 3 agents throttles the API and agents get stuck.

Verification protocol:
For every issue you suspect, you must attempt to prove yourself wrong. Do not rely on assumptions, prove the hypothesis using test cases.
DO NOT MAKE ANY DIRECT CODE CHANGES TO FIX THE ISSUES.

Output Format:
Append the confirmed issues to `audit_findings.md`, include a table at the top for the issues and then detailed issue description as separate issues.
Order the findings strictly by severity (Critical, High, Medium, Low).
For each finding, use this exact structure:
- Defect: One concise sentence describing the flaw
- Location: File path, class/function, and approximate line numbers
- Mechanism: Detailed explanation of how the bug manifests at a systems level
- Execution Trace: The hypothetical call stack, state change, or trigger condition required to reproduce this.
- Impact: What this costs the user or system (e.g., data loss, crash, RCE)
- Concrete Fix: A specific snippet of C++23 code or architectural change to resolve it

Tools Available:
The editor supports code overview and symbol mode which you can use to list all includes, symbols in c++ code without reading fully. It uses treesitter to do so.
Editor executable path: koi
Calling convention for overview mode: koi --overview --files <list-of-files>
Calling convention for symbol mode: koi --symbol-mode --files <list-of-files> --definitions
Use koi --help to know more.
You must use these tools to navigate and understand the code base. NEVER read the full file. ALWAYS first use one of the above tools to probe the source code and then use targeted reads.
Before you start the code audit, understand the output of the above tools using koi/src/piece_tree.cpp as a sample source code to run the tools against and get the output.

---
We audit findings written in docs/audit_findings.md related to smart-jump feature.
So, let's do this. You still be the code reviewer. Delegate to an opus 5 agent one issue at a time starting
from critical going to low where at the end of each fix, you review the changes, let the agent know what to
improve or mark it as pass and then move to next issue. You may group the issues to fix if you think it is the
optimal choice and speeds up fixing the code. If the code changes needed after modifications and after review are very small you may
edit them directly.

Tools Available:
The editor supports code overview and symbol mode which you can use to list all includes, symbols in c++ code without reading fully. It uses treesitter to do so.
Editor executable path: koi
Calling convention for overview mode: koi --overview --files <list-of-files>
Calling convention for symbol mode: koi --symbol-mode --files <list-of-files> --definitions
Use koi --help to know more.
You must use these tools to navigate and understand the code base. NEVER read the full file. ALWAYS first use one of the above tools to probe the source code and then use targeted reads.
Before you start the code audit, understand the output of the above tools using koi/src/piece_tree.cpp as a sample source code to run the tools against and get the output.
