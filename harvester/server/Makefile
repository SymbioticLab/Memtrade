all: wss mem_harvest cold

broker: broker.c
	gcc -lpthread -o broker broker.c

producer: producer.c
	gcc -lpthread -o producer producer.c

consumer: consumer.c
	gcc -lpthread -o consumer consumer.c

clean:
	rm broker producer consumer