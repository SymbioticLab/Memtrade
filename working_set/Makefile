all: wss mem_harvest cold

wss: wss.c
	gcc -o wss.o wss.c

mem_harvest: mem_harvest.c
	gcc -o mem_harvester.o mem_harvest.c

cold: cold_page_detector.cpp
	g++ -o cold.o cold_page_detector.cpp

clean:
	rm *.o