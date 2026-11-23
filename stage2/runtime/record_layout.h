/* Slots of the records the runtime reads, as their kaikai declarations order
 * them. Each `layout:` line names the declaration it mirrors, with its fields
 * in declaration order; each `mask:` line gives the slot-kind mask the
 * runtime builds that record with (kind 1 for a field declared Int, 2 for
 * Real, 0 for any other, two bits per field). tools/test-record-layout.sh
 * fails when a declaration and its lines disagree. The enumerators count the
 * fields of their layout line from zero. */

/* layout: stdlib/effects.kai FileHandle: fd */
/* mask: stdlib/effects.kai FileHandle: 1 */
enum { KAI_SLOT_FILEHANDLE_FD, KAI_MASK_FILEHANDLE = 1 };

/* layout: stdlib/net/tcp.kai Conn: fd */
/* mask: stdlib/net/tcp.kai Conn: 1 */
enum { KAI_SLOT_CONN_FD, KAI_MASK_CONN = 1 };

/* layout: stdlib/net/tcp.kai Listener: fd port */
/* mask: stdlib/net/tcp.kai Listener: 5 */
enum { KAI_SLOT_LISTENER_FD, KAI_SLOT_LISTENER_PORT, KAI_MASK_LISTENER = 5 };

/* layout: stdlib/net/udp.kai UdpSocket: fd port */
/* mask: stdlib/net/udp.kai UdpSocket: 5 */
enum { KAI_SLOT_UDPSOCKET_FD, KAI_SLOT_UDPSOCKET_PORT, KAI_MASK_UDPSOCKET = 5 };

/* layout: stdlib/net/udp.kai SocketAddr: host port */
/* mask: stdlib/net/udp.kai SocketAddr: 4 */
enum { KAI_SLOT_SOCKADDR_HOST, KAI_SLOT_SOCKADDR_PORT, KAI_MASK_SOCKADDR = 4 };

/* layout: stdlib/net/unix.kai Conn: fd */
/* mask: stdlib/net/unix.kai Conn: 1 */
enum { KAI_SLOT_UNIXCONN_FD };

/* layout: stdlib/net/unix.kai Listener: fd path dev ino */
/* mask: stdlib/net/unix.kai Listener: 81 */
enum {
    KAI_SLOT_UNIXLISTENER_FD,
    KAI_SLOT_UNIXLISTENER_PATH,
    KAI_SLOT_UNIXLISTENER_DEV,
    KAI_SLOT_UNIXLISTENER_INO,
    KAI_MASK_UNIXLISTENER = 81
};

/* layout: stdlib/effects/os.kai Child: pid */
/* mask: stdlib/effects/os.kai Child: 1 */
enum { KAI_SLOT_CHILD_PID, KAI_MASK_CHILD = 1 };

/* layout: stdlib/time.kai WallTime: secs nanos */
/* layout: stdlib/time.kai Instant: secs nanos */
/* mask: stdlib/time.kai WallTime: 5 */
/* mask: stdlib/time.kai Instant: 5 */
enum { KAI_SLOT_TIME_SECS, KAI_SLOT_TIME_NANOS, KAI_MASK_TIME = 5 };
