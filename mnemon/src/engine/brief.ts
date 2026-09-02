const ELLIPSIS = "\u2026";

/** Flatten whitespace runs so one memory cannot inflate a discovery row. */
export function flattenWhitespace(content: string): string {
  return content.split(/\s+/u).filter((part) => part.length > 0).join(" ");
}

/**
 * Token-friendly excerpt: flatten whitespace, truncate on a Unicode code-point
 * boundary, and append an ellipsis. Mirrors the C++/Go `--brief` projection.
 */
export function makeBriefExcerpt(content: string, maxChars: number): string {
  const flat = flattenWhitespace(content);
  const points = [...flat];
  if (points.length <= maxChars) {
    return flat;
  }
  if (maxChars === 1) {
    return ELLIPSIS;
  }
  let prefix = points.slice(0, maxChars - 1).join("");
  prefix = prefix.replace(/\s+$/u, "");
  return prefix + ELLIPSIS;
}
