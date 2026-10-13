import { ForumPage } from "@/components/forum-page";
export default async function Page({
  params,
}: {
  params: Promise<{ path?: string[] }>;
}) {
  const { path = [] } = await params;
  return <ForumPage path={path} />;
}
