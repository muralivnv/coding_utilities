Source code `koi/src` is a 17K line C++23 text editor with ZED editor like multi-buffers, treesitter based code navigation, sqlite-database based state management.
I need a deeply adversarial, highly thorough code audit. For a reference on the type of bugs that could be lurking, refer to files,

1. audit_findings.md
2. audit_findings_v2.md
3. audit_findings_v3.md
4. audit_findings_v4.md (audit currently ongoing)
which are files from previous rounds. Skip the issues which are unfixed as these will be fixed altogether in one-go.

Instructions:
Review the codebase for bugs, edge cases, and performance bottlenecks. Attack the logic adversarially.
Focus your audit on these specific vectors:
1. Focus ONLY and THOROUGHLY on the following files,
   a. project.cpp, search.cpp, symbols.cpp, editor.cpp, shell.cpp, query.cpp, textobject.cpp
2. Memory & C++ Footguns: Out-of-bounds accesses, one-off indexing errors, `nullptr` dereferences, memory leaks, and lifetime issues (e.g., dangling string_views/spans, undefined behavior).
3. Critical Paths: Database calls, file I/O, excerpt views, and hunk commits.
   - Pinpoint areas lacking robust error handling (e.g., missing `try...catch`, unchecked `std::expected` or `std::optional`, or missing fallback logic).
   - Suggest concrete implementations to prevent crashes.
4. State Corruption: Logical bugs that can lead to internal state corruption.
5. IGNORE "the TSan/release disagreement in `SearchExcerptView`" that was mentioned in audit_findings_v3. DO NOT CHASE IT.
6. Review each file using a dedicated agent with a dedicated prompt created for opus 5. YOU MUST DELEGATE and you never should read the entire file.
7. Every finding you receive, you verify it again to filter out false positives.
8. Make sure we only run 3 agents at any given time as I've seen in the past running more than 3 agents throttles the API and agents get stuck.

Verification protocol:
For every issue you suspect, you must attempt to prove yourself wrong. Do not rely on assumptions, prove the hypothesis using test cases.
DO NOT MAKE ANY DIRECT CODE CHANGES TO FIX THE ISSUES.

Output Format:
Append the confirmed issues to `audit_findings_v4.md`, include a table at the top for the issues and then detailed issue description as separate issues.
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
Editor executable path: /home/murali/local_disk/app_dev/coding_utilities/build/koi/koi
Calling convention for overview mode: /home/murali/local_disk/app_dev/coding_utilities/build/koi/koi --overview --files <list-of-files>
Calling convention for symbol mode: /home/murali/local_disk/app_dev/coding_utilities/build/koi/koi --symbol-mode --files <list-of-files> --definitions
Use /home/murali/local_disk/app_dev/coding_utilities/build/koi/koi --help to know more.
You must use these tools to navigate and understand the code base. NEVER read the full file. ALWAYS first use one of the above tools to probe the source code and then use targeted reads.
Before you start the code audit, understand the output of the above tools using koi/src/piece_tree.cpp as a sample source code to run the tools against and get the output.

---
We still have  17 Mediums and 23 Lows, plus the four latent issues the fix round itself uncovered to fix.
So, let's do this. You still be the code reviewer. And Let opus fix one issue at a time. Delegate to an opus 5
agent one issue at a time starting from medium going to low where at the end of each fix, you review the changes, let the agent know what to
improve or mark it as pass and then move to next issue. If the code changes after modifications are very small you may
edit them directly. What do you think? This way we don't need to run another expensive auditing do we?
