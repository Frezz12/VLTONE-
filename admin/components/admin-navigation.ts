import { Kanban, ShieldCheck, Activity, Bot, Bug, CircleGauge, Files, Image, MessageSquareText, PackageOpen, Users } from "lucide-react";
import type { LucideIcon } from "lucide-react";

export const adminNavigation: Array<{ label: string; links: Array<{ href: string; label: string; icon: LucideIcon; keywords: string }> }> = [
  { label: "Рабочее пространство", links: [
    { href: "/", label: "Обзор", icon: CircleGauge, keywords: "статистика dashboard онлайн токены" },
    { href: "/tasks", label: "Доска задач", icon: Kanban, keywords: "идеи задачи команда исполнитель план" },
    { href: "/team", label: "Команда", icon: ShieldCheck, keywords: "администраторы права доступ" },
    { href: "/users", label: "Пользователи", icon: Users, keywords: "аккаунт email доступ лимит" },
    { href: "/bugs", label: "Баги", icon: Bug, keywords: "ошибки репорты поддержка" },
    { href: "/crashes", label: "Краши", icon: Activity, keywords: "сбои логи диагностика" },
  ] },
  { label: "Приложение и сайт", links: [
    { href: "/releases", label: "Релизы", icon: PackageOpen, keywords: "версия обновления публикация загрузка" },
    { href: "/browser-backgrounds", label: "Фоны браузера", icon: Image, keywords: "изображение коллекция оформление" },
  ] },
  { label: "AI и система", links: [
    { href: "/models", label: "Модели AI", icon: Bot, keywords: "соединение провайдер ключ" },
    { href: "/prompts", label: "Промпты", icon: MessageSquareText, keywords: "плейбук ассистент инструкция" },
    { href: "/audit", label: "Аудит", icon: Files, keywords: "история действия журнал" },
  ] },
];
