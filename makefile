CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

all: main.cpp  ./timer/timer.cpp ./http_conn/http_conn.cpp ./log/log.cpp ./CGImariadb/sql_connection_pool.cpp  ./serversrc/server.cpp ./config/config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server