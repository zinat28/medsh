import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Semaphore;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * ExamServer - the server side of the Medical Entrance Exam system.
 *
 * It lets many clients (the medsh shell, later the JavaFX apps) book seats in
 * exam centers AT THE SAME TIME. Seats are a limited shared resource, so this
 * is where the OS concepts show up:
 *
 *   - one THREAD per connected client            (see main + handle)
 *   - a RACE CONDITION in UNSAFE mode            (see bookUnsafe)
 *   - a counting SEMAPHORE fixes it in SAFE mode (see bookSafe)
 *
 * Protocol: plain text, one command per line, e.g.
 *   SEATS | BOOK DHAKA zinat | STATS | MODE SAFE | MODE UNSAFE | RESET | PING
 *
 * Run:  java -cp server/classes ExamServer [port]      (default 5050)
 */
public class ExamServer {

    /** One exam center with a fixed number of seats. */
    static class Center {
        final String name;
        final int capacity;

        /** SAFE mode: a counting semaphore. Each permit = one free seat. */
        final Semaphore seats;

        /** UNSAFE mode: a plain int with NO protection (this is the bug on purpose). */
        int unsafeCount = 0;

        /** How many times we answered "OK" for this center (what clients were promised). */
        final AtomicInteger granted = new AtomicInteger();

        Center(String name, int capacity) {
            this.name = name;
            this.capacity = capacity;
            this.seats = new Semaphore(capacity);
        }

        void reset() {
            unsafeCount = 0;
            granted.set(0);
            seats.drainPermits();
            seats.release(capacity);
        }
    }

    static final Map<String, Center> centers = new LinkedHashMap<>();
    static final AtomicInteger clientIds = new AtomicInteger();
    static final AtomicInteger rollNumbers = new AtomicInteger(100000);
    static volatile boolean safeMode = true;

    static {
        centers.put("DHAKA", new Center("DHAKA", 20));
        centers.put("CTG",   new Center("CTG", 10));
        centers.put("RAJ",   new Center("RAJ", 10));
    }

    // ------------------------------------------------------------------
    // Booking: the interesting part
    // ------------------------------------------------------------------

    /** WRONG on purpose: read, check, then write are three separate steps. */
    static boolean bookUnsafe(Center c) {
        int seen = c.unsafeCount;            // 1. read
        if (seen < c.capacity) {             // 2. check  ("there is still a seat")
            pause();                         //    <- another thread can run right here
            c.unsafeCount = seen + 1;        // 3. write  (overwrites the other thread's update)
            return true;
        }
        return false;
    }

    /** CORRECT: tryAcquire() is atomic. It takes a permit only if one is free. */
    static boolean bookSafe(Center c) {
        return c.seats.tryAcquire();
    }

    /** Makes the race window wider so the bug shows up every time in the demo. */
    static void pause() {
        try {
            Thread.sleep(2);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    static String book(String centerName, String who) {
        Center c = centers.get(centerName.toUpperCase());
        if (c == null) return "ERR unknown center '" + centerName + "' (try: SEATS)";

        boolean ok = safeMode ? bookSafe(c) : bookUnsafe(c);
        if (!ok) return "FULL " + c.name;

        c.granted.incrementAndGet();
        int roll = rollNumbers.incrementAndGet();
        return String.format("OK %s roll=M-%06d name=%s", c.name, roll, who);
    }

    // ------------------------------------------------------------------
    // Commands
    // ------------------------------------------------------------------

    static String process(String line) {
        if (line.isEmpty()) return "ERR empty command";
        String[] p = line.split("\\s+", 3);
        String cmd = p[0].toUpperCase();

        switch (cmd) {
            case "PING":
                return "PONG";

            case "SEATS": {
                StringBuilder sb = new StringBuilder();
                for (Center c : centers.values()) {
                    int left = c.capacity - c.granted.get();
                    sb.append(String.format("%-6s capacity=%-3d booked=%-3d free=%d%n",
                            c.name, c.capacity, c.granted.get(), Math.max(left, 0)));
                }
                return sb.toString().stripTrailing();
            }

            case "BOOK": {
                if (p.length < 2) return "ERR usage: BOOK <center> [name]";
                return book(p[1], p.length > 2 ? p[2] : "anonymous");
            }

            case "STATS": {
                StringBuilder sb = new StringBuilder();
                sb.append("mode=").append(safeMode ? "SAFE (semaphore)" : "UNSAFE (no locking)").append('\n');
                for (Center c : centers.values()) {
                    int g = c.granted.get();
                    sb.append(String.format("%-6s capacity=%-3d granted=%-3d  ", c.name, c.capacity, g));
                    sb.append(g > c.capacity ? "OVERBOOKED by " + (g - c.capacity) : "OK").append('\n');
                }
                return sb.toString().stripTrailing();
            }

            case "MODE": {
                if (p.length < 2) return "ERR usage: MODE SAFE|UNSAFE";
                if (p[1].equalsIgnoreCase("SAFE")) safeMode = true;
                else if (p[1].equalsIgnoreCase("UNSAFE")) safeMode = false;
                else return "ERR usage: MODE SAFE|UNSAFE";
                return "OK mode=" + (safeMode ? "SAFE" : "UNSAFE");
            }

            case "RESET": {
                for (Center c : centers.values()) c.reset();
                rollNumbers.set(100000);
                return "OK all seats cleared";
            }

            case "HELP":
                return "commands: PING | SEATS | BOOK <center> [name] | STATS | MODE SAFE|UNSAFE | RESET";

            default:
                return "ERR unknown command '" + cmd + "' (try: HELP)";
        }
    }

    // ------------------------------------------------------------------
    // One thread per client
    // ------------------------------------------------------------------

    static void handle(Socket socket, int id) {
        try (socket;
             BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream()));
             PrintWriter out = new PrintWriter(socket.getOutputStream(), true)) {

            // ONE request per connection: read one line, answer, close.
            // (We do not wait for the client to "half-close" its side. Some forwarders,
            //  like the WSL <-> Windows localhost relay, do not pass that signal on, and the
            //  client would wait forever for an answer-complete signal.)
            String line = in.readLine();
            if (line != null) {
                String reply = process(line.trim());
                String word = line.trim().toUpperCase();
                // the GUI polls SEATS every 2 s: do not flood the log with it
                if (!word.startsWith("SEATS") && !word.startsWith("PING")) {
                    System.out.println("[server] client-" + id + " > " + line.trim() + "  =>  "
                            + reply.split("\n")[0]);
                }
                out.println(reply);
                out.flush();
            }
        } catch (IOException e) {
            System.out.println("[server] client-" + id + " error: " + e.getMessage());
        }
    }

    public static void main(String[] args) throws IOException {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 5050;
        // Loopback only by default, so nobody else on the network can reach it.
        String bind = System.getenv().getOrDefault("EXAM_BIND", "127.0.0.1");

        Runtime.getRuntime().addShutdownHook(new Thread(
                () -> System.out.println("[server] shutting down (signal received)")));

        try (ServerSocket server = new ServerSocket(port, 100, InetAddress.getByName(bind))) {
            System.out.println("[server] started, listening on " + bind + ":" + port
                    + ", mode=" + (safeMode ? "SAFE" : "UNSAFE"));
            while (true) {
                Socket client = server.accept();           // blocks until a client connects
                int id = clientIds.incrementAndGet();
                Thread t = new Thread(() -> handle(client, id), "client-" + id);
                t.start();                                 // each client gets its own thread
            }
        }
    }
}
