all: broker producer consumer

broker: broker.c
	gcc broker.c -lpthread -o broker

producer: producer.c producer.h
	gcc producer.c -lpthread -o producer

consumer: consumer.c
	gcc consumer.c -lpthread -o consumer

clean:
	rm broker producer consumer
