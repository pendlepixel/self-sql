objects = os.o
CPP = g++
CPPFLAGS = -std=c++11

os : $(objects)
	$(CPP) $(CPPFLAGS) $(objects) -o os

os.o : os.h mutex.h

.PHONY : clean

clean:
	-rm &(objects)

