export function jaccardTokenSimilarity(a: Set<string>, b: Set<string>): number {
  if (a.size === 0 || b.size === 0) {
    return 0;
  }
  const inter = intersectionCount(a, b);
  const union = a.size + b.size - inter;
  return union === 0 ? 0 : inter / union;
}

export function symmetricTokenSimilarity(a: Set<string>, b: Set<string>): number {
  if (a.size === 0 || b.size === 0) {
    return 0;
  }
  const inter = intersectionCount(a, b);
  return Math.max(inter / a.size, inter / b.size);
}

function intersectionCount(a: Set<string>, b: Set<string>): number {
  let inter = 0;
  const smaller = a.size <= b.size ? a : b;
  const larger = a.size <= b.size ? b : a;
  for (const t of smaller) {
    if (larger.has(t)) {
      inter++;
    }
  }
  return inter;
}

export function cosineSimilarity(a: readonly number[], b: readonly number[]): number {
  if (a.length === 0 || b.length === 0 || a.length !== b.length) {
    return 0;
  }
  let dot = 0;
  let na = 0;
  let nb = 0;
  for (let i = 0; i < a.length; i++) {
    const x = a[i]!;
    const y = b[i]!;
    if (!Number.isFinite(x) || !Number.isFinite(y)) {
      return 0;
    }
    dot += x * y;
    na += x * x;
    nb += y * y;
  }
  if (na === 0 || nb === 0) {
    return 0;
  }
  const result = dot / (Math.sqrt(na) * Math.sqrt(nb));
  return Number.isFinite(result) ? result : 0;
}

export function setsEqual(a: Set<string>, b: Set<string>): boolean {
  if (a.size !== b.size) {
    return false;
  }
  for (const t of a) {
    if (!b.has(t)) {
      return false;
    }
  }
  return true;
}
