/**
 * JsonEditor — CodeMirror 6 based JSON editor component.
 * Used in Insert, Edit, Query, and Console features.
 */

import { useRef, useEffect, useCallback } from 'react';
import { EditorState, Compartment } from '@codemirror/state';
import { EditorView, keymap, lineNumbers, highlightActiveLine } from '@codemirror/view';
import { defaultKeymap, indentWithTab, history, historyKeymap } from '@codemirror/commands';
import { json, jsonLanguage } from '@codemirror/lang-json';
import { syntaxHighlighting, defaultHighlightStyle, bracketMatching, indentOnInput } from '@codemirror/language';
import { autocompletion, closeBrackets } from '@codemirror/autocomplete';
import { vscodeDark } from '@uiw/codemirror-theme-vscode';
import { clsx } from 'clsx';

interface JsonEditorProps {
  value: string;
  onChange?: (value: string) => void;
  readOnly?: boolean;
  placeholder?: string;
  className?: string;
  minHeight?: number;
}

const readOnlyCompartment = new Compartment();

export function JsonEditor({ value, onChange, readOnly = false, placeholder, className, minHeight = 120 }: JsonEditorProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const viewRef = useRef<EditorView | null>(null);
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;

  useEffect(() => {
    if (!containerRef.current) return;

    const view = new EditorView({
      parent: containerRef.current,
      state: EditorState.create({
        doc: value,
        extensions: [
          vscodeDark,
          json(),
          lineNumbers(),
          highlightActiveLine(),
          history(),
          bracketMatching(),
          closeBrackets(),
          indentOnInput(),
          autocompletion(),
          syntaxHighlighting(defaultHighlightStyle),
          keymap.of([...defaultKeymap, ...historyKeymap, indentWithTab]),
          readOnlyCompartment.of(EditorState.readOnly.of(readOnly)),
          EditorView.theme({
            '&': { height: '100%', fontFamily: "'JetBrains Mono', monospace", fontSize: '12px' },
            '.cm-scroller': { overflow: 'auto' },
            '.cm-content': { minHeight: `${minHeight}px` },
          }),
          EditorView.updateListener.of(update => {
            if (update.docChanged) {
              onChangeRef.current?.(update.state.doc.toString());
            }
          }),
          ...(placeholder
            ? [EditorView.contentAttributes.of({ 'data-placeholder': placeholder })]
            : []),
        ],
      }),
    });
    viewRef.current = view;

    return () => {
      view.destroy();
      viewRef.current = null;
    };
  }, []); // mount once

  // Sync external value changes without recreating the editor
  useEffect(() => {
    const view = viewRef.current;
    if (!view) return;
    const current = view.state.doc.toString();
    if (current !== value) {
      view.dispatch({ changes: { from: 0, to: current.length, insert: value } });
    }
  }, [value]);

  // Sync readOnly changes
  useEffect(() => {
    viewRef.current?.dispatch({
      effects: readOnlyCompartment.reconfigure(EditorState.readOnly.of(readOnly)),
    });
  }, [readOnly]);

  return (
    <div
      ref={containerRef}
      className={clsx(
        'h-full w-full bg-surface-900 border border-surface-500/30 rounded-md overflow-hidden',
        className,
      )}
    />
  );
}
