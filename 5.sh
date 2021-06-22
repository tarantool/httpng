#while true; do top -b -n 1; ps ax -o ppid -o pid -o cmd; sleep 5; date; done # Ubuntu
while true; do top -l 2 -n 10; ps ax -o ppid -o pid; ps ax; sleep 5; date; done

