import { describe, expect, it } from "vitest";

import { extractEntities, extractEntitiesIndexed, mergeEntities } from "../../src/engine/entities.js";

describe("extractEntities", () => {
  it("extracts CamelCase, acronyms, paths, URLs, mentions, and CJK titles", () => {
    const text =
      'OpenTypeScript uses HTTP in src/main.ts see https://example.com/a and @alice reads 《史记》';
    const entities = extractEntities(text);
    expect(entities).toContain("OpenTypeScript");
    expect(entities).toContain("HTTP");
    expect(entities).toContain("src/main.ts");
    expect(entities).toContain("https://example.com/a");
    expect(entities).toContain("alice");
    expect(entities).toContain("史记");
  });

  it("drops acronym stopwords", () => {
    expect(extractEntities("THE AND FOR")).not.toContain("THE");
  });

  it("matches the tech dictionary with original case", () => {
    expect(extractEntities("We use TypeScript and PostgreSQL")).toEqual(
      expect.arrayContaining(["TypeScript", "PostgreSQL"]),
    );
  });

  it("merges provided entities first", () => {
    expect(mergeEntities(["Provided"], ["TypeScript", "Provided"])).toEqual(["Provided", "TypeScript"]);
  });

  it("admits known single-segment names that regex would skip", () => {
    expect(extractEntities("Talk to Hestia about the store")).not.toContain("Hestia");
    expect(extractEntitiesIndexed("Talk to Hestia about the store", new Set(["Hestia"]))).toContain("Hestia");
    expect(extractEntitiesIndexed("check mnemon later", new Set(["mnemon"]))).toContain("mnemon");
    expect(extractEntitiesIndexed("Talk to Hestia", new Set())).not.toContain("Hestia");
  });
});
