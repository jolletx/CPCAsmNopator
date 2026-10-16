# CPCAsmNopator
A python helper to provide correct CPC NOP timings inside assembly files.

# Version
- 0.1: First delivered version

# Summary
Defined to be used with CPCTelera way of coding, this script scans assembly files for each operation and provide its NOP timing (CPC measure). It also compute data size if encountered. 
New lines are automatically commented with their timings, and old lines can be updated (when a bad timing is found).
It also computes global data size and code size as a summary for each file and for directories.
It computes also macros and repeat patterns. And provides their timing / size accordingly when used. 

# AI
Copilot AI agent was used to build this tool. The starting prompt is provided inside the references directory. Conversation was long, so a lot of things have changed since. 

# Status
- It uses 2 csv with operation patterns with size and NOP timings based on ttps://cpctech.cpcwiki.de/docs/instrtim.html
- It understand sdas macro langage. 
- It works well with cpctelera sources incuding special opcodes macros.
- It provides unknown status when encountered.

# Usage
- Analysis with check of old values:
python3 CPCAsmNopator.py --update test/input.s

- Modification (only new lines):
python3 CPCAsmNopator.py --modify test/input.s

- Analysis of directory:
python3 CPCAsmNopator.py --update test
