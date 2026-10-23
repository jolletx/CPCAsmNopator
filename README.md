# CPCAsmNopator
A python and/or executable helper to provide correct CPC NOP timings inside assembly files.

# Version
- 0.2b
  - Correcting ld a,i (and variants) handling in python
- 0.2
  - Adding c implementation in parallel to help integrate in cpctelera_next
  - moving sdas instructions inside its own csv file.
  - Option --add-size to comment data size in a comment
  - Adding timings summary (do not decode, so no loops), can be useful if straightforward
- 0.1
  - First delivered version
  - Python version

# Summary
Defined to be used with CPCTelera way of coding, this script scans assembly files for each operation and provide its NOP timing (CPC measure). It also computes code and data sizes.
New lines are automatically commented with their timings, and old lines can be updated (when a bad timing is found and an option is set).
Global data size and code size are always included in summaries. Use `--data-size` to add or update byte-size comments on data directives.
It computes also macros and repeat patterns (when easily determined). And provides their timing / size accordingly when used. 

# AI
Copilot AI agent was used to build this tool. The starting prompt is provided inside the references directory. Conversation was long, so a lot of things have changed since. 

# Status
- It uses CSV tables in `input/` for instruction patterns and timings, operand substitutions, and SDAS directives.
- It understand sdas macro langage: SDAS directive behavior is defined 
in `input/SdasDirectives.csv` and shared by the Python and C implementations.
- It works well with cpctelera sources incuding special opcodes macros.
- It provides unknown status when encountered.

# Usage
- Analysis with check of old values:
python3 CPCAsmNopator.py --update test/input.s

- Modification (only new lines):
python3 CPCAsmNopator.py --modify test/input.s

- Modification with data-size comments:
python3 CPCAsmNopator.py --modify --data-size test/input.s

- Analysis of directory:
python3 CPCAsmNopator.py --update test

# C version
Build the native executable in `bin/` with:

make

Run it with the same options and arguments as the Python script, for example:

./bin/CPCAsmNopator --update test/sample.s
