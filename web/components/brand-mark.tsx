import Image from "next/image";

export function BrandMark({ className }: { className?: string }) {
  return <span className={["brand-mark", className].filter(Boolean).join(" ")} aria-hidden="true">
    <Image src="/logo.png" width={48} height={48} sizes="48px" alt="" />
  </span>;
}
