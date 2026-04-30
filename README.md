# CS4760-project4
<br>Name: Elisa Reyes
<br>Date: 04/02
<br>Environment: vi, visual studio code
<br>How to compile the project: Type 'make'
<br>Type 'make clean' to do a clean
<br> Example of how to run the project: ./oss -n 3 -s 2 -t 4 -i 0.6
<br>Generative AI used: chatgpt
<br>Prompts:
<br>Write code in C that will simulate the process scheduling part of an operating system. Implement a round-robin scheduler. (Update my code Using the pseudocode)
oss.c will fork multiple children at random times. oss acts as the scheduler and so will schedule a process by sending it a message using a message queue. If there are no processes currently ready to run in the system, it should increment the clock until it is the time where it should launch a process. It should then set up that process, generate a new time where it will create a new process and then using a message queue, schedule a process to run by sending it a message. It should then wait for a message back from that process that it has finished its task. oss now adds time to the clock. oss simulates time passing in the system by adding time to the clock and as it is the only process that would change the clock. If a user process uses some time, oss should indicate this by advancing the clock. If oss does something that should take some time if it was a real operating system, it should increment the clock by a small amount to indicate the time it spent. Only one process will be in the "running" state in our simulated system at a time as we are doing scheduling. OSS will maintain a ready queue and when new processes are launched, they should be linked into that queue. Every time oss has to make a scheduling decision it will select the process at the front of that queue. When processes become unblocked or give back control to oss, they should be put at the back of the queue. Before each scheduling decision, oss should output the ready queue.

<br>Summary: The inital generated code seemed to have good functionality, but I needed to fix a few issues with the output