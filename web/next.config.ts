import createNextIntlPlugin from "next-intl/plugin";
import type { NextConfig } from "next";

const apiOrigin = process.env.VLT_API_ORIGIN ?? "http://localhost:8080";
const config: NextConfig = {
  transpilePackages: ["@vlt/api-client", "@vlt/ui"],
  async rewrites() {
    return [{ source: "/api/:path*", destination: `${apiOrigin}/:path*` }];
  },
};

export default createNextIntlPlugin()(config);
