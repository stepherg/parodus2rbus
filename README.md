# parodus2rbus

A lightweight bridge translating simple Parodus-style JSON requests into RBUS get/set operations.

## Status
Supports Parodus (default) transport via libparodus or a mock stdin JSON mode (`--mode mock`). Handles WRP message types: REQ, RETREIVE (CRUD read), and EVENT. Incoming payloads are expected to be JSON following the simple protocol below; responses are sent back using the same WRP message type with source/dest swapped and the original transaction UUID preserved (if present).

## Build
Built as part of top-level project:
```
cmake -S . -B build
cmake --build build --parallel
```
Executable: `build/parodus2rbus/parodus2rbus`

## Usage
```
parodus2rbus [--component RBUS_NAME] [--service-name PARODUS_NAME] [--mode parodus|mock] [--log 0-3]
```
Defaults:
- mode: parodus
- component (RBUS): parodus2rbus.client
- service-name (Parodus registration): config
- PARODUS_URL (env) default tcp://127.0.0.1:6666
- PARODUS_CLIENT_URL (env) default tcp://127.0.0.1:6668

Feed line-delimited JSON requests on stdin (mock mode):
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
- GET_ATTRIBUTES: {"id","op":"GET_ATTRIBUTES","param":string}
- SET_ATTRIBUTES: {"id","op":"SET_ATTRIBUTES","param":string,"attributes":{"notify":int,"access":string}}
- ADD_ROW: {"id","op":"ADD_ROW","tableName":string,"rowData":[{"name":string,"value":string,"dataType":int}]}
- DELETE_ROW: {"id","op":"DELETE_ROW","rowName":string}
- REPLACE_ROWS: {"id","op":"REPLACE_ROWS","tableName":string,"tableData":[[{"name":string,"value":string,"dataType":int}]]}

### WebPA Compatibility
The bridge also supports WebPA-style commands that are automatically translated:
- `{"command":"GET","names":[...]}` → `{"op":"GET","params":[...]}`
- `{"command":"SET_ATTRIBUTES","parameters":[{"name":"...","attributes":{...}}]}` → `{"op":"SET_ATTRIBUTES","param":"...","attributes":{...}}`
- `{"command":"ADD_ROW","table":"...","row":[...]}` → `{"op":"ADD_ROW","tableName":"...","rowData":[...]}`

### Enhanced Features (Phase 1)
- **Table Operations**: Full support for RBUS table management (add/delete/replace rows)
- **Attribute Operations**: Get and set parameter attributes (notification settings, access control)
- **Enhanced Error Mapping**: Detailed RBUS error code mapping to HTTP status codes
- **Typed Data Support**: Proper data type handling for all parameter values

### Wildcards
You can request a group of parameters by supplying a trailing dot in a GET param, e.g.
```
{"id":"10","op":"GET","params":["Device.DeviceInfo."]}
```
Behavior: expanded via `rbus_getExt` partial path query; each returned fully qualified parameter becomes a key in `results`.
Partial paths and deeper table wildcards (e.g. `Device.IP.Interface.*.Status`) will be relayed to RBUS and expanded if the provider supports them.
Failures for an expanded path add that name with a null value and may yield multi-status (207).

When carried over Parodus:
- WRP REQ and RETREIVE payloads are interpreted identically as JSON requests.
- WRP EVENT payloads can also carry a request (useful for fire-and-forget queries); reply is sent back as EVENT.
Reply semantics:
- For REQ and RETREIVE the reply mirrors the original WRP type and transaction_uuid.
- For EVENT the reply is an EVENT with dest = original source (or a fallback) and a newly generated timestamp in the JSON body.

`status` 200 = success, 207 = partial (some parameters failed), 500 = error.

## Next Steps
- Add method invocation and event subscription forwarding.
- Smarter type handling for SET (infer numeric/bool). 
- Error mapping refinement.

## License
Same as root project.
