import { describe, expect, it } from "vitest";

import { detectIntent } from "../../src/engine/intent.js";

describe("detectIntent", () => {
  it("detects WHY when it strictly leads", () => {
    expect(detectIntent("why did we decide this")).toBe("WHY");
  });

  it("detects WHEN when it strictly leads", () => {
    expect(detectIntent("when did this happen in the timeline")).toBe("WHEN");
  });

  it("detects ENTITY when those terms appear and WHY/WHEN do not win", () => {
    expect(detectIntent("tell me about PostgreSQL")).toBe("ENTITY");
  });

  it("returns GENERAL on ties and empty signal", () => {
    expect(detectIntent("why when")).toBe("GENERAL");
    expect(detectIntent("remember the meeting notes")).toBe("GENERAL");
  });

  it("detects CJK WHY", () => {
    expect(detectIntent("为什么选择这个")).toBe("WHY");
  });
});
