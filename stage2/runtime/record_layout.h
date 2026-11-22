/* Slots of the records the runtime reads, as their kaikai declarations order
 * them. Each `layout:` line names the declaration it mirrors, with its fields
 * in declaration order; tools/test-record-layout.sh fails when a declaration
 * and its line disagree. The enumerators below count the fields of their line
 * from zero. */

/* layout: stdlib/effects.kai FileHandle: fd */
enum { KAI_SLOT_FILEHANDLE_FD };

/* layout: stdlib/net/tcp.kai Conn: fd */
enum { KAI_SLOT_CONN_FD };

/* layout: stdlib/net/tcp.kai Listener: fd port */
enum { KAI_SLOT_LISTENER_FD, KAI_SLOT_LISTENER_PORT };

/* layout: stdlib/net/udp.kai UdpSocket: fd port */
enum { KAI_SLOT_UDPSOCKET_FD, KAI_SLOT_UDPSOCKET_PORT };

/* layout: stdlib/net/udp.kai SocketAddr: host port */
enum { KAI_SLOT_SOCKADDR_HOST, KAI_SLOT_SOCKADDR_PORT };

/* layout: stdlib/net/unix.kai Conn: fd */
enum { KAI_SLOT_UNIXCONN_FD };

/* layout: stdlib/net/unix.kai Listener: fd path dev ino */
enum {
    KAI_SLOT_UNIXLISTENER_FD,
    KAI_SLOT_UNIXLISTENER_PATH,
    KAI_SLOT_UNIXLISTENER_DEV,
    KAI_SLOT_UNIXLISTENER_INO
};

/* layout: stdlib/effects/os.kai Child: pid */
enum { KAI_SLOT_CHILD_PID };

/* layout: stdlib/time.kai WallTime: secs nanos */
/* layout: stdlib/time.kai Instant: secs nanos */
enum { KAI_SLOT_TIME_SECS, KAI_SLOT_TIME_NANOS };
