from coord import *
i=int(sys.argv[1]);case=json.loads((R/'protocol-proposal.json').read_text())['cases'][i-1]
post('ASSIGNMENT',**case,require='ACK exact commands and test_id, then start both servers concurrently, verify both CM listeners and post READY; wait for clients. DONE must include full Local/Remote/combined rows and exit codes. Do not advance without coordinator acceptance.')
