import { afterEach, describe, expect, it, vi } from "vitest";

import {
  LlamaCppEmbeddingProvider,
  OllamaEmbeddingProvider,
  OpenAIEmbeddingProvider,
} from "../../src/http-embedding-provider.js";
import { MnemonEmbeddingError } from "../../src/errors.js";

describe("LlamaCppEmbeddingProvider", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    vi.unstubAllEnvs();
  });

  it("prefixes nomic text, sends dimensions, and uses encoding_format float", async () => {
    const calls: Array<{ url: string; headers: unknown; body: unknown }> = [];
    vi.stubGlobal(
      "fetch",
      async (url: string, init?: RequestInit) => {
        calls.push({ url, headers: init?.headers, body: JSON.parse(String(init?.body)) });
        return new Response(JSON.stringify({ data: [{ embedding: Array.from({ length: 768 }, () => 0.1) }] }), {
          status: 200,
        });
      },
    );
    const provider = new LlamaCppEmbeddingProvider({
      endpoint: "http://127.0.0.1:8080",
      model: "nomic-embed-text",
      dimensions: 768,
    });
    await provider.embed("hello", "document");
    await provider.embed("hello", "query");
    expect(calls.map((c) => c.url)).toEqual([
      "http://127.0.0.1:8080/v1/embeddings",
      "http://127.0.0.1:8080/v1/embeddings",
    ]);
    expect(calls.map((c) => c.body)).toEqual([
      { input: "search_document: hello", model: "nomic-embed-text", dimensions: 768, encoding_format: "float" },
      { input: "search_query: hello", model: "nomic-embed-text", dimensions: 768, encoding_format: "float" },
    ]);
    expect(provider.dimensions).toBe(768);
  });

  it("sends a bearer token and truncates Matryoshka extras", async () => {
    vi.stubEnv("MNEMON_EMBED_API_KEY", "secret-key");
    vi.stubGlobal(
      "fetch",
      async (_url: string, init?: RequestInit) => {
        expect(init?.headers).toMatchObject({ authorization: "Bearer secret-key" });
        return new Response(JSON.stringify({ data: [{ embedding: [1, 2, 3, 4] }] }), { status: 200 });
      },
    );
    const provider = new LlamaCppEmbeddingProvider({
      endpoint: "http://127.0.0.1:8080",
      dimensions: 2,
    });
    await expect(provider.embed("hello", "query")).resolves.toEqual([1, 2]);
  });

  it("throws when llama.cpp is unavailable", async () => {
    vi.stubGlobal("fetch", async () => {
      throw new Error("connect ECONNREFUSED");
    });
    const provider = new LlamaCppEmbeddingProvider();
    await expect(provider.embed("hello", "query")).rejects.toBeInstanceOf(MnemonEmbeddingError);
  });
});

describe("OpenAIEmbeddingProvider", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("posts raw text to /v1/embeddings without nomic prefixes", async () => {
    const calls: Array<{ url: string; body: unknown }> = [];
    vi.stubGlobal(
      "fetch",
      async (url: string, init?: RequestInit) => {
        calls.push({ url, body: JSON.parse(String(init?.body)) });
        return new Response(JSON.stringify({ data: [{ embedding: [0.1, 0.2] }] }), { status: 200 });
      },
    );
    const provider = new OpenAIEmbeddingProvider({
      endpoint: "http://127.0.0.1:4000/v1",
      model: "text-embedding-3-small",
      dimensions: 2,
    });
    await provider.embed("hello", "query");
    expect(calls).toEqual([
      {
        url: "http://127.0.0.1:4000/v1/embeddings",
        body: { input: "hello", model: "text-embedding-3-small", dimensions: 2 },
      },
    ]);
  });
});

describe("OllamaEmbeddingProvider", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    vi.unstubAllEnvs();
  });

  it("posts raw text to /api/embed without nomic prefixes", async () => {
    const calls: Array<{ url: string; body: unknown }> = [];
    vi.stubGlobal(
      "fetch",
      async (url: string, init?: RequestInit) => {
        calls.push({ url, body: JSON.parse(String(init?.body)) });
        return new Response(JSON.stringify({ embeddings: [Array.from({ length: 768 }, () => 0.1)] }), {
          status: 200,
        });
      },
    );
    const provider = new OllamaEmbeddingProvider({
      endpoint: "http://127.0.0.1:11434",
      model: "nomic-embed-text",
      dimensions: 768,
    });
    await provider.embed("hello", "query");
    expect(calls).toEqual([
      {
        url: "http://127.0.0.1:11434/api/embed",
        body: { model: "nomic-embed-text", input: "hello" },
      },
    ]);
  });
});
