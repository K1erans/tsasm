export const readerActions = [
  "ParseStartSmall",
  "ParseStartLarge",
  "SetLimitLow",
  "SetLimitDefault",
  "SetRetainZero",
  "SetRetainDefault",
  "ParserFree",
];

export const groupColors = {
  "Idle within retain cap": "#bfe3df",
  "Idle holding big parse output": "#f0dcbf",
  "Parsing in progress": "#c3e0ff",
  "Finishing or unwinding parse": "#e3d5ff",
};

export function group(v) {
  if (v.phase === "idle") {
    return v.workReleased ? "Idle holding big parse output" : "Idle within retain cap";
  }
  if (v.phase === "parsing") return "Parsing in progress";
  return "Finishing or unwinding parse";
}

export function state(v) {
  const work =
    v.workCap === 0
      ? "no work buffers"
      : v.workCap === 1
        ? "heap work buffers"
        : "mapped work buffers";
  const out =
    v.outCap === 0
      ? "no output array"
      : v.outCap === 1
        ? "heap output array"
        : "mapped output array";
  if (v.phase === "idle") {
    if (v.workReleased) {
      return "Idle: work buffers released after big parse, output array held for caller";
    }
    if (v.workCap === 0 && v.outCap === 0) {
      return "Idle: all buffers empty";
    }
    return `Idle: retained ${work} and ${out}`;
  }
  if (v.phase === "parsing") {
    if (v.workReleased) {
      return "Parsing: previous big parse output array still pinned";
    }
    return `Parsing: ${work}, ${out}`;
  }
  if (v.phase === "ret_ok") {
    return v.memUsed > v.retain
      ? "Finishing parse: total memory above retain cap"
      : "Finishing parse: total memory within retain cap";
  }
  return v.memUsed > v.retain
    ? "Unwinding failed parse: total memory above retain cap"
    : "Unwinding failed parse: total memory within retain cap";
}

export function step(action) {
  switch (action) {
    case "ParseStartSmall":
      return "start parse on small input";
    case "ParseStartLarge":
      return "start parse on large input";
    case "RejectOversizeLen":
      return "reject input at or above 4 GiB";
    case "AllocNodeFast":
      return "allocate tree node within budget";
    case "GrowNodes":
      return "grow tree node arena";
    case "GrowWorkSmall":
      return "grow work buffers on heap";
    case "GrowWorkMapMin":
      return "grow work buffers to anonymous mapping";
    case "GrowOOM":
      return "allocator fails during buffer growth";
    case "EmitSmallOut":
      return "emit output array on heap";
    case "EmitLargeOut":
      return "emit output array in anonymous mapping";
    case "EmitAbortAfterGrow":
      return "abort during output emission after growing output array";
    case "SyntaxError":
      return "abort parse on syntax error";
    case "MainRetOk":
      return "return from successful parse";
    case "MainRetErr":
      return "return from failed parse";
    case "SetLimitLow":
      return "lower memory limit";
    case "SetLimitDefault":
      return "restore default memory limit";
    case "SetRetainZero":
      return "lower retain cap to zero";
    case "SetRetainDefault":
      return "restore default retain cap";
    case "ParserFree":
      return "free parser buffers";
    default:
      return action;
  }
}
