import { UserDetail } from "@/components/user-detail";
export default async function User({ params }: { params: Promise<{ id: string }> }) { const { id } = await params; return <UserDetail id={id} />; }
