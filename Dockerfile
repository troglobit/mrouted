# You probably don't want to use this, becuase mrouted need the actual
# interfaces in its network namespace to work, meaning you wont see them
# anymore on the host side ...
FROM alpine:latest

# Build depends
RUN apk add --no-cache git gcc musl-dev linux-headers make pkgconfig	\
			automake autoconf bison flex

COPY . /tmp/mrouted
RUN git clone --depth=1 file:///tmp/mrouted /root/mrouted
WORKDIR /root/mrouted

RUN	./autogen.sh && 						 \
	./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
		    --without-systemd			 		 \
	make && 							 \
	make install-strip

FROM alpine:latest
COPY --from=0 /usr/sbin/mrouted /usr/sbin/mrouted

CMD [ "/usr/sbin/mrouted" ]
