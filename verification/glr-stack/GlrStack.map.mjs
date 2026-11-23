export const readerActions = [];

export const groupColors = {
  "Single active version": "#bfe3df",
  "Multiple GLR versions active": "#c3e0ff",
  "Merged DAG head with extension block": "#e3d5ff",
  "Stack nodes recycled on free list": "#f0dcbf",
};

export function group(v) {
  if (v.sfree.length > 0) return "Stack nodes recycled on free list";
  if (v.nh > 1) return "Multiple GLR versions active";
  if (v.extOwned.length > 0) return "Merged DAG head with extension block";
  return "Single active version";
}

export function state(v) {
  const free =
    v.sfree.length > 0 ? "with recycled nodes on free list" : "free list empty";
  if (v.nh === 1) {
    if (v.h1Node === 0 && v.extOwned.length === 0) {
      return `Single version at base node (${free})`;
    }
    if (v.extOwned.includes(v.h1Node)) {
      return `Single version at merged DAG head with extension block (${free})`;
    }
    if (v.extOwned.length > 0) {
      return `Single version with uncollected extension block awaiting GC (${free})`;
    }
    return `Single version on linear stack chain (${free})`;
  }
  if (v.h1Dead || v.h2Dead) {
    return `Two GLR versions, one dead awaiting condense (${free})`;
  }
  if (v.h1Node === v.h2Node) {
    return `Two live GLR versions sharing head node (${free})`;
  }
  return `Two live GLR versions on distinct branches (${free})`;
}

export function step(action) {
  switch (action) {
    case "PushV1":
      return "shift or push stack node on primary version";
    case "ForkV2":
      return "fork second GLR version";
    case "PushV2":
      return "shift or push stack node on second version";
    case "MergeV2IntoV1":
      return "merge second version into primary head via extension block";
    case "TryAddSelfLink":
      return "ignore self-loop link in link attachment";
    case "ReduceInPlace":
      return "reduce single-link chain in place and recycle node";
    case "ReduceMergedGSS":
      return "reduce across merged DAG node via iterator slices";
    case "ShareHeadWithV2":
      return "fork second version sharing primary head";
    case "MarkV1Dead":
      return "mark primary version dead on no action";
    case "MarkV2Dead":
      return "mark second version dead on no action";
    case "Condense":
      return "condense stack and drop dead versions";
    case "RunGC":
      return "mark live stack nodes from heads and sweep unreachable nodes";
    default:
      return action;
  }
}
