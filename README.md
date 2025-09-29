# parodus2rbus

A lightweight bridge translating simple Parodus-style JSON requests into RBUS get/set operations.

## Status
Initial scaffold: supports mock stdin JSON transport only.

## Build
Built as part of top-level project:
```
cmake -S . -B build
cmake --build build --parallel
```
Executable: `build/parodus2rbus/parodus2rbus`

## Usage
```
parodus2rbus [--component NAME] [--mode mock] [--log 0-3]
```

Feed line-delimited JSON requests on stdin:
```
{"id":"1","op":"GET","params":["Device.DeviceInfo.SerialNumber"]}
{"id":"2","op":"SET","param":"Device.DeviceInfo.SerialNumber","value":"ABC123"}
```

Responses:
```
{"id":"1","status":200,"results":{"Device.DeviceInfo.SerialNumber":"123456789"}}
{"id":"2","status":200,"message":"OK"}
```

## Protocol
- GET: {"id","op":"GET","params":[..]}
- SET: {"id","op":"SET","param":string,"value":string}

`status` 200 = success, 207 = partial (some parameters failed), 500 = error.

## Next Steps
- Integrate real libparodus transport.
- Add method invocation and event subscription forwarding.
- Smarter type handling for SET (infer numeric/bool). 
- Error mapping refinement.

## License
Same as root project.
