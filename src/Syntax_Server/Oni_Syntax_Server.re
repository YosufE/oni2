/*
 Syntax Server
 */

module Protocol = Oni_Syntax.Protocol;
module ClientToServer = Protocol.ClientToServer;

module Transport = Exthost.Transport;

type message =
  | Log(string)
  | Message(ClientToServer.t)
  | Exception(string);

let start = (~healthCheck) => {
  let transport = ref(None);

  let write = (msg: Protocol.ServerToClient.t) => {
    let bytes = Marshal.to_bytes(msg, []);
    let packet = Transport.Packet.create(~bytes, ~packetType=Regular, ~id=0);

    transport^ |> Option.iter(Transport.send(~packet));
  };

  let log = msg => write(Protocol.ServerToClient.Log(msg));
  let logError = exn =>
    write(
      Protocol.ServerToClient.Log(
        exn |> Printexc.to_string |> (str => "ERROR: " ++ str),
      ),
    );

  let parentPid = Unix.getenv("__ONI2_PARENT_PID__") |> int_of_string;
  let namedPipe = Unix.getenv("__ONI2_NAMED_PIPE__");

  log("Starting up server. Parent PID is: " ++ string_of_int(parentPid));

  let state = ref(State.empty);
  let timer: Luv.Timer.t = Luv.Timer.init() |> Result.get_ok;

  let stopWork = () => Luv.Timer.stop(timer);
  let map2 = f => state := f(state^);

  let doWork = () =>
    try(
      {
        if (State.anyPendingWork(state^)) {
          //log("Running unit of work...");
          map2(
            State.doPendingWork,
            //log("Unit of work completed.");
          );
        } else {
          //log("No pending work, stopping.");
          let _: result(unit, Luv.Error.t) = stopWork();
          ();
        };

        let tokenUpdates = State.getTokenUpdates(state^);
        write(Protocol.ServerToClient.TokenUpdate(tokenUpdates));
        //log("Token updates sent.");
        map2(State.clearTokenUpdates);
      }
    ) {
    | ex =>
      logError(ex);
      exit(2);
    };

  let startWork = () => {
    Luv.Timer.start(~repeat=1, timer, 0, () => {doWork()}) |> Result.get_ok;
  };

  let map = f => {
    state := f(state^);
    //if (State.anyPendingWork(state^)) {
    let _: result(unit, Luv.Error.t) = Luv.Timer.again(timer);
    ();
    //();
  };

  startWork();

  let handleProtocol =
    ClientToServer.(
      fun
      | Echo(m) => {
          write(Protocol.ServerToClient.EchoReply(m));
          log("handled echo");
        }
      | Initialize(languageInfo, setup) => {
          map(State.initialize(~log, languageInfo, setup));
          write(Protocol.ServerToClient.Initialized);
          log("Initialized!");
        }
      | RunHealthCheck => {
          let res = healthCheck();
          write(Protocol.ServerToClient.HealthCheckPass(res == 0));
        }
      | BufferEnter(id, filetype) => {
          log(
            Printf.sprintf(
              "!!!!!!!! Buffer enter - id: %d filetype: %s",
              id,
              filetype,
            ),
          );
          map(State.bufferEnter(id));
        }
      | ConfigurationChanged(config) => {
          map(State.updateConfiguration(config));
          let treeSitterEnabled =
            Oni_Core.Configuration.getValue(
              c => c.experimentalTreeSitter,
              config,
            );
          log(
            "got new config - treesitter enabled:"
            ++ (treeSitterEnabled ? "true" : "false"),
          );
        }
      | ThemeChanged(theme) => {
          map(State.updateTheme(theme));
          log("handled theme changed");
        }
      | BufferUpdate(bufferUpdate, lines, scope) => {
          map(State.bufferUpdate(~bufferUpdate, ~lines, ~scope));
          log(
            Printf.sprintf(
              "Received buffer update - %d | %d lines",
              bufferUpdate.id,
              Array.length(lines),
            ),
          );
        }
      | VisibleRangesChanged(visibilityUpdate) => {
          map(State.updateVisibility(visibilityUpdate));
        }
      | Close => {
          write(Protocol.ServerToClient.Closing);
          exit(0);
        }
      | SimulateMessageException => failwith("Exception!")
      | v => log("Unhandled message: " ++ ClientToServer.show(v))
    );

  let handleMessage =
    fun
    | Log(msg) => log(msg)
    | Exception(msg) => log("Exception encountered: " ++ msg)
    | Message(protocol) => handleProtocol(protocol);

  let handlePacket = ({body, _}: Transport.Packet.t) => {
    let msg: Protocol.ClientToServer.t = Marshal.from_bytes(body, 0);

    try(handleMessage(Message(msg))) {
    | exn =>
      logError(exn);
      exit(2);
    };
  };

  let waitForPidWindows = pid =>
    try({
      let (_exitCode, _status) = Thread.wait_pid(pid);
      exit(0);
    }) {
    // If the PID doesn't exist, Thread.wait_pid will throw
    | _ex => exit(2)
    };

  let waitForPidPosix = pid => {
    while (true) {
      Unix.sleepf(5.0);
      try(Unix.kill(pid, 0)) {
      // If we couldn't send signal 0, the process is dead:
      // https://stackoverflow.com/questions/3043978/how-to-check-if-a-process-id-pid-exists
      | _ex => exit(0)
      };
    };
  };

  let waitForPid = Sys.win32 ? waitForPidWindows : waitForPidPosix;

  let _parentProcessWatcherThread: Thread.t =
    Thread.create(() => {waitForPid(parentPid)}, ());

  let dispatch =
    fun
    | Transport.Connected => log("Connected!")
    | Transport.Received(packet) => handlePacket(packet)
    | _ => ();

  let transportResult = Exthost.Transport.connect(~namedPipe, ~dispatch);

  transport := transportResult |> Result.to_option;

  let _: bool = Luv.Loop.run();
  ();
};
