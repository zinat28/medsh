# medsh – OS creative project starter

A Unix shell written in C that also controls the Medical Entrance Exam system.

## Folder layout (put everything under ONE folder)

```
~/os-project/
├── medsh/                     <- this folder (unzip here)
│   ├── medsh.c                <- the whole shell (edit this file)
│   ├── Makefile               <- `make` builds, `make test` tests
│   ├── scripts/fake_exam_server.sh   <- placeholder server (replace later with Java)
│   ├── tests/run_tests.sh     <- automated tests
│   └── run/                   <- created automatically: exam.pid, exam.log
└── exam-system/               <- your OOP JavaFX project (copy it here)
```

## Build and run

```bash
cd ~/os-project/medsh
make            # compiles medsh.c  -> ./medsh
make test       # 23 automated tests
./medsh         # start your shell
```

## Try these inside medsh

```
medsh> help
medsh> ls -l | grep medsh | wc -l
medsh> echo hello > a.txt ; cat a.txt
medsh> sleep 100          # then press Ctrl-Z, then:  jobs   bg   fg   Ctrl-C
medsh> sleep 5 &
medsh> exam start
medsh> exam status
medsh> exam logs
medsh> exam stop
```

## Where each OS concept lives in medsh.c

| Concept | Function |
|---|---|
| fork / exec / wait | `execute_pipeline`, `wait_job_fg` |
| pipes, dup2 | `execute_pipeline` (child branch) |
| redirection | `apply_redirs` |
| process groups, job control | `execute_pipeline`, `give_terminal_to`, `builtin_fg/bg` |
| signals | `setup_shell`, `reset_signals`, `update_job` |
| daemonizing the server | `exam_start` (setsid) |
| parsing | `tokenize`, `run_line` |

## Your exercises (do these in order, they are your own contribution)

1. `cd -`, `~` expansion (`cd ~/x`), and `echo $?`.
2. Wildcards (`ls *.c`) with `glob(3)`.
3. `$VAR` expansion, `export VAR=value` (use `setenv`).
4. Replace `history` with arrow-key history using GNU readline (`-lreadline`).
5. `exam seats` and `exam results`: connect a TCP socket to the Java server
   (`socket`, `connect`, `send`, `recv`) and print the reply.
6. `loadtest N`: fork N client processes that connect at the same time.
7. Real Java server: set `MEDSH_EXAM_CMD="java -jar /path/to/server.jar"`.

## Two fixes for the Java (OOP) project

1. `HelloApplication.java`: change `"/.../login.fxml"` to `"/.../Login.fxml"`
   (Linux file names are case-sensitive).
2. `LoginController.loginButtonOnAction`: replace the body with

```java
String type = userTypeComboBox.getValue();
if (type == null) { messegeLabel.setText("Please select a user type"); return; }
for (Login x : LoginManager.getLoginArrayList()) {
    if (usernameTextField.getText().equals(x.getUsername())
            && passwordTextField.getText().equals(x.getPassword())
            && type.equals(x.getUserType())) {
        messegeLabel.setText("Login Successful");
        String base = "/com/summer/section1/group4/medical_entrance_exam/zinat/";
        SceneSwitcher.switchTo(type.equals("Applicant")
                ? base + "Applicant/Applicant.fxml"
                : base + "QuestionModerator/QuestionModerator.fxml");
        return;
    }
}
messegeLabel.setText("Login Failed");
```

---

# Step 2: real Java server + race-condition demo

New files: `server/ExamServer.java`, `scripts/exam_server.sh`.
Changed: `medsh.c` (socket client, new `exam` subcommands, `loadtest`), `tests/run_tests.sh`.

Needs Java: `sudo apt install default-jdk`. Add to `.gitignore`: `server/classes/`

## The demo (your 5-minute show)

```
medsh> exam start                 # compiles + starts the Java server in the background
medsh> exam seats                 # DHAKA 20 seats, CTG 10, RAJ 10
medsh> exam book DHAKA zinat      # one booking
medsh> exam mode unsafe           # turn OFF locking (the bug, on purpose)
medsh> exam reset
medsh> exam loadtest 40           # 40 processes book DHAKA at the same instant
                                  #   -> DHAKA capacity=20 granted=40 OVERBOOKED by 20
medsh> exam mode safe             # turn ON the semaphore
medsh> exam reset
medsh> exam loadtest 40
                                  #   -> DHAKA capacity=20 granted=20 OK
medsh> exam stats | grep DHAKA    # works because of your own pipes
medsh> exam logs 20               # see every client thread in the server log
medsh> exam stop
```

## Where each OS concept lives

| Concept | Where |
|---|---|
| Thread per client | `ExamServer.main` (`new Thread(...)`) and `handle` |
| Race condition (check-then-act) | `ExamServer.bookUnsafe` |
| Counting semaphore (the fix) | `ExamServer.bookSafe` (`Semaphore.tryAcquire`) |
| Sockets, client side | `medsh.c`: `connect_server`, `exam_request` |
| Pipe used as a start barrier | `medsh.c`: `exam_loadtest` (children block in `read` until the parent closes the pipe) |
| Many processes at once | `exam_loadtest` (`fork` x N, `waitpid`) |
| Detached background daemon | `exam_start` (`setsid`) |
| Why "stopped" = pid gone AND port closed | `exam_stop` |

## Honest note for your report

`bookUnsafe` sleeps 2 ms between the check and the write to WIDEN the race window,
so the bug shows up every time. Without the pause the bug is real but rare.
Say this in your report: it shows you understand that races are timing-dependent.
