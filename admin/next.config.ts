import type { NextConfig } from "next";
const apiOrigin = (process.env.VLT_API_ORIGIN ?? "http://localhost:8080")
  .replace(/\/+$/, "")
  .replace(/\/v1$/, "");
const config: NextConfig = {
  output: "standalone",
  transpilePackages: ["@vlt/api-client", "@vlt/ui"],
  async rewrites() { return [{ source: "/api/:path*", destination: `${apiOrigin}/:path*` }]; },
};
export default config;
