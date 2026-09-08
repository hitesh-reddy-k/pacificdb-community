const MAX_QUERY_LIMIT = Number(process.env.PACIFICDB_QUERY_MAX_LIMIT || 1000);
const MAX_AGGREGATION_INPUT = Number(process.env.PACIFICDB_AGGREGATION_MAX_INPUT || 5000);

function toNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function normalizeLimit(value, fallback = 100, max = MAX_QUERY_LIMIT) {
  return Math.max(1, Math.min(max, Math.floor(toNumber(value, fallback))));
}

function normalizeSkip(value) {
  return Math.max(0, Math.floor(toNumber(value, 0)));
}

function normalizeProjection(projection) {
  if (!projection) return null;
  if (Array.isArray(projection)) {
    return projection.reduce((acc, field) => {
      if (field) acc[String(field)] = 1;
      return acc;
    }, {});
  }
  if (typeof projection === 'string') {
    return normalizeProjection(projection.split(',').map((s) => s.trim()).filter(Boolean));
  }
  if (typeof projection === 'object') return projection;
  return null;
}

function normalizeSort(sort) {
  if (!sort) return null;
  if (Array.isArray(sort)) {
    return sort.map((entry) => Array.isArray(entry)
      ? [String(entry[0]), Number(entry[1]) < 0 ? -1 : 1]
      : [String(entry), 1]);
  }
  if (typeof sort === 'string') {
    return sort.split(',').map((part) => {
      const trimmed = part.trim();
      if (!trimmed) return null;
      if (trimmed.startsWith('-')) return [trimmed.slice(1), -1];
      return [trimmed.replace(/^\+/, ''), 1];
    }).filter(Boolean);
  }
  if (typeof sort === 'object') {
    return Object.entries(sort).map(([field, dir]) => [field, Number(dir) < 0 ? -1 : 1]);
  }
  return null;
}

function getPath(doc, dotted) {
  return String(dotted).split('.').reduce((cur, key) => (cur == null ? undefined : cur[key]), doc);
}

function compareValues(a, b) {
  if (a === b) return 0;
  if (a == null) return -1;
  if (b == null) return 1;
  return a < b ? -1 : 1;
}

function sortDocuments(docs, sort) {
  const normalized = normalizeSort(sort);
  if (!normalized || !normalized.length) return docs;
  return [...docs].sort((a, b) => {
    for (const [field, dir] of normalized) {
      const cmp = compareValues(getPath(a, field), getPath(b, field));
      if (cmp !== 0) return cmp * dir;
    }
    return 0;
  });
}

function matchesCondition(actual, condition) {
  if (!condition || typeof condition !== 'object' || Array.isArray(condition)) {
    return actual === condition;
  }
  for (const [op, expected] of Object.entries(condition)) {
    if (op === '$eq' && actual !== expected) return false;
    if (op === '$ne' && actual === expected) return false;
    if (op === '$gt' && !(actual > expected)) return false;
    if (op === '$gte' && !(actual >= expected)) return false;
    if (op === '$lt' && !(actual < expected)) return false;
    if (op === '$lte' && !(actual <= expected)) return false;
    if (op === '$in') {
      if (!Array.isArray(expected)) return false;
      const hit = Array.isArray(actual) ? actual.some((item) => expected.includes(item)) : expected.includes(actual);
      if (!hit) return false;
    }
    if (op === '$nin' && Array.isArray(expected)) {
      const hit = Array.isArray(actual) ? actual.some((item) => expected.includes(item)) : expected.includes(actual);
      if (hit) return false;
    }
  }
  return true;
}

function matchesFilter(doc, filter = {}) {
  for (const [field, condition] of Object.entries(filter || {})) {
    if (field === '$and') {
      if (!Array.isArray(condition) || !condition.every((f) => matchesFilter(doc, f))) return false;
      continue;
    }
    if (field === '$or') {
      if (!Array.isArray(condition) || !condition.some((f) => matchesFilter(doc, f))) return false;
      continue;
    }
    if (!matchesCondition(getPath(doc, field), condition)) return false;
  }
  return true;
}

function applyProjection(doc, projection) {
  const normalized = normalizeProjection(projection);
  if (!normalized) return doc;
  const entries = Object.entries(normalized);
  const include = entries.some(([, v]) => Number(v) === 1 || v === true);
  if (include) {
    const out = {};
    for (const [field, flag] of entries) {
      if (Number(flag) === 1 || flag === true) {
        const value = getPath(doc, field);
        if (value !== undefined) out[field] = value;
      }
    }
    if (doc.id != null && normalized.id !== 0 && normalized._id !== 0) out.id = doc.id;
    return out;
  }
  const out = { ...doc };
  for (const [field, flag] of entries) {
    if (Number(flag) === 0 || flag === false) delete out[field];
  }
  return out;
}

function validateUpdateDocument(update) {
  if (!update || typeof update !== 'object' || Array.isArray(update)) {
    return { ok: false, reason: 'update_object_required' };
  }
  const operators = Object.keys(update).filter((key) => key.startsWith('$'));
  if (!operators.length) return { ok: true, mode: 'replacement' };
  const allowed = new Set(['$set', '$inc', '$unset', '$push', '$pull', '$mul']);
  const unsupported = operators.filter((op) => !allowed.has(op));
  if (unsupported.length) return { ok: false, reason: 'unsupported_update_operator', unsupported };
  return { ok: true, mode: 'operators', operators };
}

function normalizeFindRequest(body = {}) {
  const limit = normalizeLimit(body.limit);
  const skip = normalizeSkip(body.skip);
  return {
    query: body.query || body.filter || {},
    limit,
    skip,
    sort: normalizeSort(body.sort),
    projection: normalizeProjection(body.projection),
    timeoutMs: body.timeoutMs ? Math.max(1, Math.min(30000, Number(body.timeoutMs))) : null,
    memoryLimitBytes: body.memoryLimitBytes ? Math.max(1024, Number(body.memoryLimitBytes)) : null,
    explain: body.explain === true,
  };
}

function applyPipeline(inputDocs, pipeline = []) {
  if (!Array.isArray(pipeline)) {
    return { ok: false, statusCode: 400, reason: 'pipeline_array_required' };
  }
  let docs = inputDocs.slice(0, MAX_AGGREGATION_INPUT);
  const explain = [];
  for (const stage of pipeline) {
    const [[op, spec] = []] = Object.entries(stage || {});
    if (!op) return { ok: false, statusCode: 400, reason: 'invalid_stage' };
    if (op === '$match') docs = docs.filter((doc) => matchesFilter(doc, spec || {}));
    else if (op === '$project') docs = docs.map((doc) => applyProjection(doc, spec || {}));
    else if (op === '$sort') docs = sortDocuments(docs, spec || {});
    else if (op === '$limit') docs = docs.slice(0, normalizeLimit(spec, 100, MAX_AGGREGATION_INPUT));
    else if (op === '$skip') docs = docs.slice(normalizeSkip(spec));
    else if (op === '$count') docs = [{ [String(spec || 'count')]: docs.length }];
    else {
      return { ok: false, statusCode: 501, reason: 'aggregation_stage_not_certified', stage: op };
    }
    explain.push({ stage: op, output: docs.length });
  }
  return { ok: true, documents: docs, explain };
}

function explainFind({ query, limit, skip, sort, projection }, returnedDocuments, elapsedMs) {
  return {
    selectedIndex: null,
    indexUseCertified: false,
    scannedDocuments: null,
    returnedDocuments,
    executionTimeMs: elapsedMs,
    sortMethod: sort && sort.length ? 'engine_or_gateway_sort' : 'none',
    filterPushdown: true,
    projectionApplied: !!projection,
    limit,
    skip,
    warnings: [
      'Index selection and scan counts are not reported by this helper.',
      ...(query && query.$text ? ['Full-text search is unavailable.'] : []),
    ],
  };
}

module.exports = {
  MAX_QUERY_LIMIT,
  MAX_AGGREGATION_INPUT,
  normalizeLimit,
  normalizeSkip,
  normalizeProjection,
  normalizeSort,
  sortDocuments,
  matchesFilter,
  applyProjection,
  validateUpdateDocument,
  normalizeFindRequest,
  applyPipeline,
  explainFind,
};
