const MAX_LIMIT = 100;
const FIELD = '[A-Za-z_][A-Za-z0-9_.-]*';

function scalar(raw) {
    const value = String(raw).trim().replace(/^['"]|['"]$/g, '');
    if (/^-?\d+(\.\d+)?$/.test(value)) return Number(value);
    if (/^(true|false)$/i.test(value)) return value.toLowerCase() === 'true';
    if (/^null$/i.test(value)) return null;
    return value;
}
function parseClause(clause) {
    const patterns = [
        [new RegExp(`^(${FIELD})\\s+(?:is\\s+)?(?:greater than or equal to|at least|>=)\\s+(.+)$`, 'i'), '$gte'],
        [new RegExp(`^(${FIELD})\\s+(?:is\\s+)?(?:less than or equal to|at most|<=)\\s+(.+)$`, 'i'), '$lte'],
        [new RegExp(`^(${FIELD})\\s+(?:is\\s+)?(?:greater than|above|>)\\s+(.+)$`, 'i'), '$gt'],
        [new RegExp(`^(${FIELD})\\s+(?:is\\s+)?(?:less than|below|<)\\s+(.+)$`, 'i'), '$lt'],
        [new RegExp(`^(${FIELD})\\s+(?:is\\s+)?(?:not equal to|is not|!=)\\s+(.+)$`, 'i'), '$ne'],
        [new RegExp(`^(${FIELD})\\s+(?:is|equals|equal to|=)\\s+(.+)$`, 'i'), '$eq'],
    ];
    for (const [pattern, op] of patterns) {
        const hit = clause.trim().match(pattern); if (!hit) continue;
        return { field: hit[1], condition: op === '$eq' ? scalar(hit[2]) : { [op]: scalar(hit[2]) } };
    }
    return null;
}
function compileEnglishQuery(question, options = {}) {
    const source = String(question || '').trim();
    if (!source || source.length > 1000) throw Object.assign(new Error('Question must be 1-1000 characters'), { code: 'INVALID_QUESTION' });
    if (/\b(delete|drop|remove|update|insert|write|truncate)\b/i.test(source)) throw Object.assign(new Error('Natural-language console is read-only'), { code: 'READ_ONLY_NL_QUERY' });
    const limitHit = source.match(/\b(?:top|first|limit)\s+(\d{1,4})\b/i);
    const limit = Math.max(1, Math.min(MAX_LIMIT, Number(limitHit?.[1] || options.limit || 20)));
    const sortHit = source.match(new RegExp(`\\b(?:sort(?:ed)?|order(?:ed)?)\\s+by\\s+(${FIELD})(?:\\s+(ascending|descending|asc|desc))?`, 'i'));
    const sort = sortHit ? { [sortHit[1]]: /desc/i.test(sortHit[2] || '') ? -1 : 1 } : null;
    const whereHit = source.match(/\b(?:where|with|whose)\s+(.+?)(?=\s+(?:sort(?:ed)?|order(?:ed)?)\s+by\b|$)/i);
    const clauses = whereHit ? whereHit[1].split(/\s+and\s+/i).map(parseClause) : [];
    if (whereHit && clauses.some((entry) => !entry)) throw Object.assign(new Error('I could not safely translate every condition. Use examples like "where status is active and amount greater than 100".'), { code: 'AMBIGUOUS_NL_QUERY' });
    const query = {};
    for (const entry of clauses) query[entry.field] = entry.condition;
    return { operation: 'find', query, limit, sort, readOnly: true, interpretation: { filters: clauses.length, sortedBy: sortHit?.[1] || null } };
}
module.exports = { compileEnglishQuery };
