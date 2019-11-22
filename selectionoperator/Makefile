CC = g++
CFLAGS += -O3 -march=native -std=c++11 -Wall -m64 -Wno-unused-result -Wno-unused-value -Wno-unused-function -Wno-cpp -g
TESTS = ${PWD}/
DEPENDENCIES = ${PWD}/*.hpp
APP=main_selection


${APP} : ${TESTS}${APP}.cpp ${DEPENDENCIES}
	${CC} ${TESTS}${APP}.cpp -o $@ ${CFLAGS} -L${PWD} -lpthread

clean:
	/bin/rm -f ${APP}