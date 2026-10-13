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
