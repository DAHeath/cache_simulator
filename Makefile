CXX=g++
CXXFLAGS= -g -std=c++11 -O3 -I./include
LIBS= -L./lib -L./lib64 -lTask -lz
OBJECTS= cachesim.o cachesim_driver.o TraceWrapper.o

all: cachesim

cachesim: $(OBJECTS)
	$(CXX) -o cachesim $(OBJECTS) $(LIBS)

clean:
	rm -f cachesim *.o
