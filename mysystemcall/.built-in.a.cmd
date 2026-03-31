cmd_mysystemcall/built-in.a := rm -f mysystemcall/built-in.a; echo pubsub.o | sed -E 's:([^ ]+):mysystemcall/\1:g' | xargs ar cDPrST mysystemcall/built-in.a
