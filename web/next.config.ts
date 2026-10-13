import createNextIntlPlugin from "next-intl/plugin";
import type { NextConfig } from "next";

const apiOrigin = (process.env.VLT_API_ORIGIN ?? "http://localhost:8080")
  .replace(/\/+$/, "")
  .replace(/\/v1$/, "");
const config: NextConfig = {
  allowedDevOrigins: ["127.0.0.1", "localhost"],
  output: "standalone",
  images: { unoptimized: true },
  transpilePackages: ["@vlt/api-client", "@vlt/ui"],
  async rewrites() {
    return [{ source: "/api/:path*", destination: `${apiOrigin}/:path*` }];
  },
};

export default createNextIntlPlugin()(config);
