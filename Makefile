CXXFLAGS+=-std=c++20

out/wlr-gamma-daemon: src/main.cpp out/wlr-gamma-control-unstable-v1.h out/wlr-gamma-control-unstable-v1.o out/ipc.o
	mkdir -p out
	${CXX} ${CXXFLAGS} -Iout -lwayland-client -lm -o $@ src/main.cpp out/wlr-gamma-control-unstable-v1.o out/ipc.o

out/ipc.o: src/ipc.cpp
	mkdir -p out
	${CXX} ${CFLAGS} -c -o $@  $<

out/zwlr_gamma_control_manager_v1_interface.o: out/wlr-gamma-control-unstable-v1.c
	mkdir -p out
	${CC} ${CFLAGS} -c -o $@  $<

out/wlr-gamma-control-unstable-v1.h: src/wlr-gamma-control-unstable-v1.xml
	mkdir -p out
	wayland-scanner client-header $< $@

out/wlr-gamma-control-unstable-v1.c: src/wlr-gamma-control-unstable-v1.xml
	mkdir -p out
	wayland-scanner private-code $< $@
